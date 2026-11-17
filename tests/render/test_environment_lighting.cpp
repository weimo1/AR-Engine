#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

#include "render/EnvironmentLighting.h"

TEST_CASE(
    "environment bake preserves constant radiance and cube layout",
    "[render-environment-lighting]") {
    using ar::render::EnvironmentPreset;
    const auto constant = ar::render::bakeEnvironmentLighting(EnvironmentPreset::Constant);
    REQUIRE(constant.diffuse.extent == 16);
    REQUIRE(constant.diffuse.texels.size() == 6 * 16 * 16);
    const auto checkConstant = [](const auto& mip) {
        for (const auto& texel : mip.texels) {
            REQUIRE(std::abs(texel[0] - 0.25F) < 1e-5F);
            REQUIRE(std::abs(texel[1] - 0.5F) < 1e-5F);
            REQUIRE(std::abs(texel[2] - 0.75F) < 1e-5F);
            REQUIRE(texel[3] == 1.F);
        }
    };
    checkConstant(constant.diffuse);
    for (std::size_t mip = 0; mip < constant.specular.size(); ++mip) {
        const auto extent = 16u >> mip;
        REQUIRE(constant.specular[mip].extent == extent);
        REQUIRE(constant.specular[mip].texels.size() == 6 * extent * extent);
        checkConstant(constant.specular[mip]);
    }
    REQUIRE(constant.brdf.size() == 32 * 32);
    for (const auto& sample : constant.brdf) {
        REQUIRE(std::isfinite(sample[0]));
        REQUIRE(std::isfinite(sample[1]));
        REQUIRE(sample[0] >= 0.F);
        REQUIRE(sample[1] >= 0.F);
        REQUIRE(sample[0] + sample[1] <= 1.001F);
    }

    const auto directional = ar::render::bakeEnvironmentLighting(EnvironmentPreset::Directional);
    const auto& sharp = directional.specular.front().texels;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto positive = (axis * 2) * 16 * 16 + 8 * 16 + 8;
        const auto negative = positive + 16 * 16;
        REQUIRE(sharp[positive][axis] > 0.99F);
        REQUIRE(sharp[negative][axis] < 0.01F);
        REQUIRE(directional.specular.back().texels[axis * 2][axis] < sharp[positive][axis]);
    }
    REQUIRE(sharp[8 * 16 + 2][2] > sharp[8 * 16 + 13][2]); // +X face points toward +Z on the left.
}
