#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>

#include "core/Error.h"
#include "render/StaticRenderInput.h"

namespace ar::render {

struct SceneWorldBounds {
    std::array<float, 3> min{-1.F, -1.F, -1.F};
    std::array<float, 3> max{1.F, 1.F, 1.F};

    bool operator==(const SceneWorldBounds&) const = default;
};

struct DirectionalShadowConfig {
    std::uint32_t resolution = 2048u;
    float normalBias = 0.02F;
    float depthBias = 0.0005F;
    float pcfRadiusTexels = 1.0F;

    bool operator==(const DirectionalShadowConfig&) const = default;
};

struct EffectiveDirectionalLight {
    // True only when neither scene directional_light nor shadow pipeline is enabled, preserving
    // the legacy view-space headlamp L_view = (0, 0, 1) for existing non-shadow tests.
    bool legacyViewSpace = true;
    // Unit world-space propagation direction (from light toward scene), valid when
    // !legacyViewSpace.
    std::array<float, 3> directionWorld{0.F, 0.F, -1.F};
    std::array<float, 3> color{1.F, 1.F, 1.F};
    float intensity = 6.0F;
    bool castShadows = false;

    bool operator==(const EffectiveDirectionalLight&) const = default;
};

struct DirectionalShadowView {
    std::array<float, 16> lightView{};
    std::array<float, 16> lightProj{};
    std::array<float, 16> lightViewProj{};
    std::array<float, 3> lightEye{};
    float zNear = 0.1F;
    float zFar = 1.0F;
    std::uint32_t resolution = 2048u;
    float normalBias = 0.02F;
    float depthBias = 0.0005F;
    float pcfRadiusTexels = 1.0F;

    bool operator==(const DirectionalShadowView&) const = default;
};

inline constexpr std::uint32_t kMinShadowResolution = 64u;
inline constexpr std::uint32_t kMaxShadowResolution = 4096u;

[[nodiscard]] DirectionalLight defaultDirectionalLight() noexcept;

[[nodiscard]] std::expected<void, core::Error>
validateDirectionalLight(const DirectionalLight& light);

[[nodiscard]] std::expected<EffectiveDirectionalLight, core::Error> resolveDirectionalLight(
    const std::optional<DirectionalLight>& sceneLight, bool shadowPipelineEnabled);

[[nodiscard]] std::expected<SceneWorldBounds, core::Error>
computeSceneWorldBounds(const StaticRenderInput& input);

[[nodiscard]] std::expected<DirectionalShadowView, core::Error> buildDirectionalShadowView(
    const SceneWorldBounds& bounds,
    const DirectionalLight& light,
    const DirectionalShadowConfig& config = {});

} // namespace ar::render
