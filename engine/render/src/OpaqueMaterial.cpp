#include "render/OpaqueMaterial.h"

#include <cmath>

#include "assets/StaticAssetCache.h"

namespace ar::render {
namespace {
float unorm(std::uint8_t value) {
    return static_cast<float>(value) / 255.F;
}

float srgb(std::uint8_t value) {
    const auto encoded = unorm(value);
    if (encoded <= 0.04045F) {
        return encoded / 12.92F;
    }
    return std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}
} // namespace

OpaqueSurfaceSample evaluateOpaqueMaterial(
    const assets::OpaqueMaterial& material,
    std::array<std::uint8_t, 4> baseColor,
    std::array<std::uint8_t, 4> metallicRoughness) {
    const auto& factor = material.baseColorFactor;
    return {
        {factor[0] * srgb(baseColor[0]), factor[1] * srgb(baseColor[1]),
         factor[2] * srgb(baseColor[2]), factor[3] * unorm(baseColor[3])},
        material.metallicFactor * unorm(metallicRoughness[2]),
        material.roughnessFactor * unorm(metallicRoughness[1])};
}

} // namespace ar::render
