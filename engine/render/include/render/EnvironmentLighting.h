#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ar::render {

enum class EnvironmentPreset { Constant, Directional };

struct EnvironmentCubeMip {
    std::uint32_t extent;
    // Face-major, then rows and columns. Faces follow Vulkan +X, -X, +Y, -Y, +Z, -Z.
    std::vector<std::array<float, 4>> texels;
};

struct EnvironmentLightingData {
    static constexpr std::uint32_t brdfExtent = 32;
    EnvironmentCubeMip diffuse;
    std::array<EnvironmentCubeMip, 5> specular;
    std::vector<std::array<float, 2>> brdf;
};

EnvironmentLightingData bakeEnvironmentLighting(EnvironmentPreset preset);

struct PackedEnvironmentLighting {
    std::vector<std::uint16_t> diffuse;
    std::vector<std::uint16_t> specular;
    std::vector<std::uint16_t> brdf;
};

PackedEnvironmentLighting packEnvironmentLighting(const EnvironmentLightingData& environment);

} // namespace ar::render
