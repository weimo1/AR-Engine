#pragma once

#include <array>
#include <cstdint>

namespace ar::assets {
struct OpaqueMaterial;
}

namespace ar::render {

struct OpaqueSurfaceSample {
    std::array<float, 4> baseColor;
    float metallic;
    float roughness;
};

// CPU reference for shader probes. Arguments contain raw RGBA8 texels in their original encoding.
OpaqueSurfaceSample evaluateOpaqueMaterial(
    const assets::OpaqueMaterial& material,
    std::array<std::uint8_t, 4> baseColor = {255, 255, 255, 255},
    std::array<std::uint8_t, 4> metallicRoughness = {255, 255, 255, 255});

} // namespace ar::render
