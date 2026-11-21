#include <catch2/catch_test_macros.hpp>

#include <array>

#include "render/RenderGraph.h"
#include "render/PresentationGraph.h"

namespace {

using ar::render::GraphAccess;
using ar::render::GraphErrorCode;
using ar::render::GraphImage;
using ar::render::GraphPass;
using ar::render::GraphUsage;
using ar::render::planRenderGraph;
using ar::render::usageMask;

constexpr std::array images{GraphImage{2, 2}};
constexpr GraphAccess write{0, 0, 0, GraphUsage::ColorWrite};
constexpr GraphAccess fragmentRead{0, 0, 0, GraphUsage::FragmentSample};
constexpr GraphAccess computeRead{0, 0, 0, GraphUsage::ComputeSample};
constexpr GraphAccess discardWrite{0, 0, 0, GraphUsage::ColorWrite, true};

} // namespace

TEST_CASE("presentation graph orders indirect buffer consumers", "[render-graph-plan]") {
    using namespace ar::rhi;
    PresentationGraphInput input{};
    input.indirect = true;
    auto plan = ar::render::planPresentationGraph(input);
    REQUIRE(plan);
    const auto& cull = plan->indirectBarriers[static_cast<std::size_t>(IndirectPhase::Cull)];
    REQUIRE(cull.size() == 1);
    CHECK(cull.front().resource == IndirectBuffer::Counts);
    CHECK(cull.front().source == IndirectUse::Clear);
    CHECK(cull.front().destination == IndirectUse::ComputeReadWrite);
    const auto& draw = plan->indirectBarriers[static_cast<std::size_t>(IndirectPhase::Geometry)];
    REQUIRE(draw.size() == 3);
    CHECK(draw[0].resource == IndirectBuffer::Commands);
    CHECK(draw[0].source == IndirectUse::ComputeWrite);
    CHECK(draw[0].destination == IndirectUse::DrawRead);
    CHECK(draw[2].resource == IndirectBuffer::Counts);
    CHECK(draw[2].source == IndirectUse::Clear);
    constexpr auto readback = static_cast<std::size_t>(IndirectPhase::Readback);
    CHECK(plan->indirectBarriers[readback].empty());
    input.capture = true;
    plan = ar::render::planPresentationGraph(input);
    REQUIRE(plan);
    REQUIRE(plan->indirectBarriers[readback].size() == 3);
    CHECK(plan->indirectBarriers[readback][0].destination == IndirectUse::HostRead);
}

TEST_CASE("graph preserves both compatible readers before another write", "[render-graph-plan]") {
    const std::array passes{
        GraphPass{std::span{&write, 1}},
        GraphPass{std::span{&fragmentRead, 1}},
        GraphPass{std::span{&computeRead, 1}},
        GraphPass{std::span{&write, 1}}};
    const auto plan = planRenderGraph({images, passes});
    REQUIRE(plan.has_value());
    REQUIRE(plan->beforePass.size() == passes.size());
    REQUIRE(plan->beforePass[2].size() == 1);
    const auto& secondReader = plan->beforePass[2].front();
    CHECK(secondReader.previousLayout == ar::render::GraphLayout::ShaderRead);
    CHECK(secondReader.nextUsages == usageMask(GraphUsage::ComputeSample));
    CHECK((secondReader.priorUsages & usageMask(GraphUsage::ColorWrite)) != 0);
    REQUIRE(plan->beforePass[3].size() == 1);
    const auto& barrier = plan->beforePass[3].front();
    CHECK(barrier.target.image == 0);
    CHECK(barrier.target.mip == 0);
    CHECK(barrier.target.layer == 0);
    CHECK(barrier.target.usage == GraphUsage::ColorWrite);
    CHECK(
        barrier.priorUsages ==
        (usageMask(GraphUsage::FragmentSample) | usageMask(GraphUsage::ComputeSample)));
}

TEST_CASE("graph discard still waits for the earlier access", "[render-graph-plan]") {
    const std::array passes{
        GraphPass{std::span{&write, 1}},
        GraphPass{std::span{&fragmentRead, 1}},
        GraphPass{std::span{&discardWrite, 1}}};
    const auto plan = planRenderGraph({images, passes});
    REQUIRE(plan.has_value());
    REQUIRE(plan->beforePass.size() == passes.size());
    REQUIRE(plan->beforePass[2].size() == 1);
    const auto& barrier = plan->beforePass[2].front();
    CHECK(barrier.target.discardContents);
    CHECK(barrier.priorUsages == usageMask(GraphUsage::FragmentSample));
}

TEST_CASE("graph diagnoses invalid accesses at their subresource", "[render-graph-plan]") {
    const auto checkError = [](std::span<const GraphPass> passes,
                               GraphErrorCode expected,
                               std::uint32_t mip,
                               std::uint32_t layer) {
        const auto plan = planRenderGraph({images, passes});
        REQUIRE_FALSE(plan.has_value());
        CHECK(plan.error().code == expected);
        REQUIRE(plan.error().pass == 1);
        CHECK(plan.error().image == 0);
        CHECK(plan.error().mip == mip);
        CHECK(plan.error().layer == layer);
    };

    SECTION("a write initializes only its own mip and layer") {
        for (const auto& unread : std::array{
                 GraphAccess{0, 1, 0, GraphUsage::FragmentSample},
                 GraphAccess{0, 0, 1, GraphUsage::FragmentSample}}) {
            const std::array passes{
                GraphPass{std::span{&write, 1}}, GraphPass{std::span{&unread, 1}}};
            checkError(passes, GraphErrorCode::UninitializedRead, unread.mip, unread.layer);
        }
    }
    SECTION("overlapping read and write in one pass") {
        const std::array overlap{fragmentRead, write};
        const std::array passes{GraphPass{std::span{&write, 1}}, GraphPass{overlap}};
        checkError(passes, GraphErrorCode::ConflictingAccess, 0, 0);
    }
    SECTION("mip and layer bounds") {
        for (const auto& invalid : std::array{
                 GraphAccess{0, 2, 0, GraphUsage::ColorWrite},
                 GraphAccess{0, 0, 2, GraphUsage::ColorWrite}}) {
            const std::array passes{
                GraphPass{std::span{&write, 1}}, GraphPass{std::span{&invalid, 1}}};
            checkError(passes, GraphErrorCode::OutOfRange, invalid.mip, invalid.layer);
        }
    }
}

TEST_CASE("graph imports and exports external image state per subresource", "[render-graph-plan]") {
    using ar::render::GraphLayout;
    using ar::render::GraphSubresourceState;
    constexpr std::array imports{
        GraphSubresourceState{0, 0, 0, GraphLayout::Present, usageMask(GraphUsage::Present)}};
    constexpr GraphAccess copied{0, 0, 0, GraphUsage::CopyRead};
    constexpr GraphAccess presented{0, 0, 0, GraphUsage::Present};
    const std::array passes{
        GraphPass{std::span{&discardWrite, 1}},
        GraphPass{std::span{&copied, 1}},
        GraphPass{std::span{&presented, 1}}};
    const auto plan = planRenderGraph({images, passes, imports});
    REQUIRE(plan.has_value());
    REQUIRE(plan->beforePass[0].size() == 1);
    CHECK(plan->beforePass[0][0].previousLayout == GraphLayout::Present);
    CHECK(plan->beforePass[0][0].priorUsages == usageMask(GraphUsage::Present));
    REQUIRE(plan->beforePass[1].size() == 1);
    CHECK(plan->beforePass[1][0].nextLayout == GraphLayout::CopySource);
    REQUIRE(plan->finalStates.size() == 1);
    CHECK(plan->finalStates[0].layout == GraphLayout::Present);
    CHECK(plan->finalStates[0].usages == usageMask(GraphUsage::Present));
}
TEST_CASE("graph requires and validates indirect reads", "[render-graph-plan]") {
    const std::array missing{GraphPass{std::span{&write, 1}}, GraphPass{{}, true}};
    const auto rejected = planRenderGraph({images, missing});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == GraphErrorCode::MissingIndirectReads);
    CHECK(rejected.error().pass == 1);

    const std::array declared{missing[0], GraphPass{{}, true, std::span{&fragmentRead, 1}}};
    const auto plan = planRenderGraph({images, declared});
    REQUIRE(plan.has_value());
    REQUIRE(plan->beforePass[1].size() == 1);
    CHECK(plan->beforePass[1][0].nextUsages == usageMask(GraphUsage::FragmentSample));
}

TEST_CASE("graph validates depth image usage", "[render-graph-plan]") {
    constexpr std::array depthImages{GraphImage{1, 1, ar::render::GraphAspect::Depth}};
    constexpr GraphAccess depthWrite{0, 0, 0, GraphUsage::DepthWrite};
    const std::array depthPasses{
        GraphPass{std::span{&depthWrite, 1}}, GraphPass{std::span{&fragmentRead, 1}}};
    const auto plan = planRenderGraph({depthImages, depthPasses});
    REQUIRE(plan.has_value());
    REQUIRE(plan->beforePass[1].size() == 1);
    CHECK(plan->beforePass[1][0].previousLayout == ar::render::GraphLayout::DepthAttachment);

    const std::array invalid{GraphPass{std::span{&write, 1}}};
    const auto rejected = planRenderGraph({depthImages, invalid});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == GraphErrorCode::InvalidAccess);
}

TEST_CASE("graph rejects duplicate and incompatible imports", "[render-graph-plan]") {
    using ar::render::GraphError;
    using ar::render::GraphLayout;
    using ar::render::GraphSubresourceState;
    constexpr GraphSubresourceState imported{
        0, 1, 1, GraphLayout::ShaderRead, usageMask(GraphUsage::FragmentSample)};
    constexpr std::array repeated{imported, imported};
    constexpr std::array invalid{
        GraphSubresourceState{0, 1, 1, GraphLayout::ShaderRead, usageMask(GraphUsage::ColorWrite)}};
    for (const auto imports :
         {std::span<const GraphSubresourceState>{repeated},
          std::span<const GraphSubresourceState>{invalid}}) {
        const auto result = planRenderGraph({images, {}, imports});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == GraphErrorCode::InvalidImport);
        CHECK(result.error().pass == GraphError::noPass);
        CHECK(result.error().mip == 1);
        CHECK(result.error().layer == 1);
    }
    const std::array passes{GraphPass{std::span{&fragmentRead, 1}}};
    const auto result = planRenderGraph({images, passes, std::span{&imported, 1}});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == GraphErrorCode::UninitializedRead);
    CHECK(result.error().mip == 0);
    CHECK(result.error().layer == 0);
}
