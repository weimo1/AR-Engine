#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include "render/StaticSceneRenderer.h"
#include "render/StaticSceneVisibility.h"

namespace {

std::array<float, 16> makeModel(
    float tx,
    float ty,
    float tz,
    float sx = 1.F,
    float sy = 1.F,
    float sz = 1.F,
    float rotZRad = 0.F) {
    const float c = std::cos(rotZRad);
    const float s = std::sin(rotZRad);
    // Column-major T * Rz * S.
    return {
        c * sx,
        s * sx,
        0.F,
        0.F,
        -s * sy,
        c * sy,
        0.F,
        0.F,
        0.F,
        0.F,
        sz,
        0.F,
        tx,
        ty,
        tz,
        1.F,
    };
}

ar::rhi::CpuCullInstance makeBoxInstance(
    std::uint32_t index,
    const std::array<float, 16>& model,
    std::array<float, 3> bmin = {-0.5F, -0.5F, -0.5F},
    std::array<float, 3> bmax = {0.5F, 0.5F, 0.5F}) {
    return ar::rhi::CpuCullInstance{
        model, bmin, bmax, index * 36u, 36u, static_cast<std::int32_t>(index * 24u), index};
}

} // namespace

TEST_CASE(
    "CPU AABB frustum culling matches clip-space bounds across rotation, non-uniform scale, "
    "grazing edges, zero-visible, and capacity clamping",
    "[render-static-scene-visibility]") {
    using namespace ar;

    // Camera at origin looking down -Z with 90 deg vertical FOV, near=0.5, far=20.0.
    // At aspect=2.0 (inverseAspect=0.5): half-height at z=-d is d, half-width is 2*d.
    render::StaticCamera cam{};
    cam.position = {0.F, 0.F, 0.F};
    cam.orientation = {0.F, 0.F, 0.F, 1.F};
    cam.verticalFovRadians = std::numbers::pi_v<float> * 0.5F;
    cam.nearPlane = 0.5F;
    cam.farPlane = 20.F;
    const auto view = render::buildFrameView(cam, true);
    REQUIRE(view.has_value());
    constexpr float inverseAspect = 0.5F; // 2:1 viewport

    // 0: Centered inside frustum at z = -5.
    const auto inside = makeBoxInstance(0u, makeModel(0.F, 0.F, -5.F));
    // 1: Behind camera at z = +2.
    const auto behind = makeBoxInstance(1u, makeModel(0.F, 0.F, 2.F));
    // 2: Beyond far plane at z = -22 (half-extent 0.5 => nearest z = -21.5 < -20).
    const auto beyondFar = makeBoxInstance(2u, makeModel(0.F, 0.F, -22.F));
    // 3: Outside right plane at z in [-5.5, -4.5] (widest right edge at z=-5.5 is x=+11.0;
    // box at x=+12.0 spans [11.5, 12.5], strictly outside).
    const auto outsideRight = makeBoxInstance(3u, makeModel(12.F, 0.F, -5.F));
    // 4: Grazing right plane: center at x=+11.4, half=0.5 => touches x=+10.9 < 11.0 at z=-5.5.
    const auto grazingRight = makeBoxInstance(4u, makeModel(11.4F, 0.F, -5.F));
    // 5: Non-uniformly scaled bar centered outside right plane at x=+12.5, scaleX=4.0 =>
    // spans [10.5, 14.5] (unscaled [12.0, 13.0] would be culled), must be visible!
    const auto scaledReachIn = makeBoxInstance(5u, makeModel(12.5F, 0.F, -5.F, 4.F, 1.F, 1.F));
    // 6: Rotated 45 deg around Z at x=+11.65, y=0, z=-5: unrotated x_min=11.15 > 11.0 (culled),
    // rotated corner reaches 11.65 - 0.5*sqrt(2) ~= 10.943 < 11.0 at z=-5.5 => visible!
    const auto rotatedReachIn = makeBoxInstance(
        6u, makeModel(11.65F, 0.F, -5.F, 1.F, 1.F, 1.F, std::numbers::pi_v<float> * 0.25F));

    const std::vector<rhi::CpuCullInstance> instances{
        inside, behind, beyondFar, outsideRight, grazingRight, scaledReachIn, rotatedReachIn};
    std::vector<rhi::IndexedIndirectDrawCommand> commands(instances.size());

    const auto full =
        render::cullStaticSceneCpu(instances, view->clipFromWorld, inverseAspect, commands);
    CHECK(full.visibleCount == 4u);
    CHECK(full.drawCount == 4u);
    REQUIRE(full.drawCount == 4u);
    CHECK(commands[0].firstInstance == 0u);
    CHECK(commands[1].firstInstance == 4u);
    CHECK(commands[2].firstInstance == 5u);
    CHECK(commands[3].firstInstance == 6u);
    for (std::uint32_t i = 0u; i < full.drawCount; ++i) {
        const auto idx = commands[i].firstInstance;
        CHECK(commands[i].indexCount == 36u);
        CHECK(commands[i].instanceCount == 1u);
        CHECK(commands[i].firstIndex == idx * 36u);
        CHECK(commands[i].vertexOffset == static_cast<std::int32_t>(idx * 24u));
    }

    // Aspect change: at 1:1 aspect (inverseAspect=1.0), right plane at z=-5 is x=+5 so instances
    // 4, 5, 6 are now culled and only instance 0 remains visible.
    const auto narrow = render::cullStaticSceneCpu(instances, view->clipFromWorld, 1.0F, commands);
    CHECK(narrow.visibleCount == 1u);
    CHECK(narrow.drawCount == 1u);
    CHECK(commands[0].firstInstance == 0u);

    // Capacity overflow: visibleCount stays exact (4) while drawCount clamps to buffer size (2).
    std::array<rhi::IndexedIndirectDrawCommand, 2> smallOut{};
    const auto clamped =
        render::cullStaticSceneCpu(instances, view->clipFromWorld, inverseAspect, smallOut);
    CHECK(clamped.visibleCount == 4u);
    CHECK(clamped.drawCount == 2u);
    CHECK(smallOut[0].firstInstance == 0u);
    CHECK(smallOut[1].firstInstance == 4u);

    // Zero visible: all instances behind camera or outside frustum.
    const std::vector<rhi::CpuCullInstance> allCulled{behind, beyondFar, outsideRight};
    const auto empty =
        render::cullStaticSceneCpu(allCulled, view->clipFromWorld, inverseAspect, commands);
    CHECK(empty.visibleCount == 0u);
    CHECK(empty.drawCount == 0u);

    // Invalid inverseAspect returns zero cleanly.
    const auto badAspect =
        render::cullStaticSceneCpu(instances, view->clipFromWorld, 0.F, commands);
    CHECK(badAspect.visibleCount == 0u);
    CHECK(badAspect.drawCount == 0u);
}
