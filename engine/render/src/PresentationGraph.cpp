#include "render/PresentationGraph.h"

#include <array>
#include <string>
#include <utility>

#include "render/RenderGraph.h"

namespace ar::render {

std::expected<rhi::PresentationGraphPlan, core::Error>
planPresentationGraph(const rhi::PresentationGraphInput& input) {
    if (input.shadows && !input.lighting) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Validation,
                "presentation graph shadows require deferred lighting to be enabled",
                {}});
    }
    using Image = rhi::PresentationImage;
    using Pass = rhi::PresentationPass;
    constexpr auto color = GraphUsage::ColorWrite;
    constexpr auto sample = GraphUsage::FragmentSample;
    constexpr auto copy = GraphUsage::CopyRead;
    const auto access = [](Image image, GraphUsage usage, bool discard = false) {
        return GraphAccess{std::to_underlying(image), 0, 0, usage, discard};
    };
    constexpr std::array images{
        GraphImage{},
        GraphImage{},
        GraphImage{},
        GraphImage{},
        GraphImage{1, 1, GraphAspect::Depth},
        GraphImage{},
        GraphImage{1, 1, GraphAspect::Depth},
        GraphImage{1, 6},
        GraphImage{5, 6},
        GraphImage{},
    };
    std::vector<GraphSubresourceState> imports;
    for (std::uint32_t image = 0; image < input.states.size(); ++image) {
        const auto state = input.states[image];
        if (state.layout != GraphLayout::Undefined) {
            imports.push_back({image, 0, 0, state.layout, state.usages});
        }
    }

    const std::array shadow{
        access(Image::ShadowDepth, GraphUsage::DepthWrite, true),
    };
    const std::array geometry{
        access(Image::Albedo, color, true),
        access(Image::Normal, color, true),
        access(Image::Material, color, true),
        access(Image::Depth, GraphUsage::DepthWrite, true),
        access(Image::Output, color, true),
    };
    const std::array lighting{
        access(Image::Albedo, sample),
        access(Image::Normal, sample),
        access(Image::Material, sample),
        access(Image::Depth, sample),
        access(Image::Hdr, color, true),
        access(Image::ShadowDepth, sample),
    };
    std::array<GraphAccess, 37> environment{};
    std::size_t environmentCount = 0;
    if (input.environment) {
        for (std::uint32_t image = std::to_underlying(Image::Count); image < images.size();
             ++image) {
            for (std::uint32_t mip = 0; mip < images[image].mipCount; ++mip) {
                for (std::uint32_t layer = 0; layer < images[image].layerCount; ++layer) {
                    imports.push_back(
                        {image, mip, layer, GraphLayout::ShaderRead, usageMask(sample)});
                    environment[environmentCount++] = {image, mip, layer, sample};
                }
            }
        }
    }
    const std::array display{access(Image::Hdr, sample), access(Image::Output, color, true)};
    const std::array readback{
        access(Image::Output, copy),
        access(Image::Albedo, copy),
        access(Image::Normal, copy),
        access(Image::Material, copy),
        access(Image::Depth, copy),
        access(Image::Hdr, copy),
    };
    const std::array present{access(Image::Output, GraphUsage::Present)};
    const std::array passes{
        GraphPass{
            input.shadows ? std::span<const GraphAccess>(shadow) : std::span<const GraphAccess>{}},
        GraphPass{std::span(geometry).first(input.lighting ? 4 : 5)},
        GraphPass{
            input.lighting ? std::span(lighting).first(input.shadows ? 6 : 5)
                           : std::span<const GraphAccess>{},
            input.environment,
            std::span(environment).first(environmentCount)},
        GraphPass{
            input.lighting ? std::span<const GraphAccess>(display)
                           : std::span<const GraphAccess>{}},
        GraphPass{std::span(readback).first(input.capture ? (input.lighting ? 6 : 5) : 0)},
        GraphPass{present},
    };
    const auto plan = planRenderGraph({images, passes, imports});
    if (!plan) {
        const auto& error = plan.error();
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "presentation graph rejected pass " + std::to_string(error.pass) + ", image " +
                    std::to_string(error.image) + ", mip " + std::to_string(error.mip) +
                    ", layer " + std::to_string(error.layer) + ", reason " +
                    std::to_string(std::to_underlying(error.code)),
                {}});
    }
    rhi::PresentationGraphPlan result{};
    if (input.indirect) {
        using Buffer = rhi::IndirectBuffer;
        using Use = rhi::IndirectUse;
        using Phase = rhi::IndirectPhase;
        result.indirectBarriers[std::to_underlying(Phase::Cull)] = {
            {Buffer::Counts, Use::Clear, Use::ComputeReadWrite}};
        result.indirectBarriers[std::to_underlying(Phase::Geometry)] = {
            {Buffer::Commands, Use::ComputeWrite, Use::DrawRead},
            {Buffer::Counts, Use::ComputeReadWrite, Use::DrawRead},
            {Buffer::Counts, Use::Clear, Use::DrawRead}};
        if (input.capture) {
            result.indirectBarriers[std::to_underlying(Phase::Readback)] = {
                {Buffer::Commands, Use::ComputeWrite, Use::HostRead},
                {Buffer::Counts, Use::ComputeReadWrite, Use::HostRead},
                {Buffer::Counts, Use::Clear, Use::HostRead}};
        }
    }
    static_assert(std::tuple_size_v<decltype(passes)> == std::to_underlying(Pass::Count));
    for (std::size_t pass = 0; pass < passes.size(); ++pass) {
        for (const auto& barrier : plan->beforePass[pass]) {
            result.barriers[pass].push_back(
                {static_cast<Image>(barrier.target.image),
                 barrier.previousLayout,
                 barrier.nextLayout,
                 barrier.priorUsages,
                 barrier.nextUsages,
                 barrier.target.discardContents});
        }
    }
    for (const auto& resource : plan->finalStates) {
        if (resource.image < result.finalStates.size()) {
            result.finalStates[resource.image] = {resource.layout, resource.usages};
        }
    }
    return result;
}

} // namespace ar::render
