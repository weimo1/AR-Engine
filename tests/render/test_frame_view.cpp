#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include "core/Error.h"
#include "render/StaticSceneRenderer.h"

namespace {

std::array<float, 4>
mulMat4Vec4(const std::array<float, 16>& matrix, const std::array<float, 4>& vector) {
    std::array<float, 4> out{};
    for (std::size_t row = 0; row < 4; ++row) {
        out[row] = matrix[row] * vector[0] + matrix[4 + row] * vector[1] +
                   matrix[8 + row] * vector[2] + matrix[12 + row] * vector[3];
    }
    return out;
}

std::array<float, 3>
mulUpperMat3Vec3(const std::array<float, 16>& matrix, const std::array<float, 3>& vector) {
    std::array<float, 3> out{};
    for (std::size_t row = 0; row < 3; ++row) {
        out[row] =
            matrix[row] * vector[0] + matrix[4 + row] * vector[1] + matrix[8 + row] * vector[2];
    }
    return out;
}

} // namespace

TEST_CASE(
    "buildFrameView computes Reverse-Z, Y-flip, and orthonormal camera transforms",
    "[render-frame-view]") {
    ar::render::StaticCamera camera;
    camera.position = {0.F, 0.F, 5.F};
    camera.orientation = {0.F, 0.F, 0.F, 1.F};
    camera.verticalFovRadians = std::numbers::pi_v<float> * 0.5F;
    camera.nearPlane = 0.5F;
    camera.farPlane = 50.F;

    SECTION("GBuffer Reverse-Z view preserves +Y up and inverts projection") {
        const auto built = ar::render::buildFrameView(camera, true);
        REQUIRE(built.has_value());

        const auto viewOrigin = mulMat4Vec4(built->view, {0.F, 0.F, 5.F, 1.F});
        CHECK(viewOrigin[0] == Catch::Approx(0.F).margin(1e-6F));
        CHECK(viewOrigin[1] == Catch::Approx(0.F).margin(1e-6F));
        CHECK(viewOrigin[2] == Catch::Approx(0.F).margin(1e-6F));
        CHECK(viewOrigin[3] == Catch::Approx(1.F).margin(1e-6F));

        const auto nearClip = mulMat4Vec4(built->clipFromWorld, {0.F, 0.F, 4.5F, 1.F});
        CHECK(nearClip[2] / nearClip[3] == Catch::Approx(1.F).margin(1e-5F));

        const auto farClip = mulMat4Vec4(built->clipFromWorld, {0.F, 0.F, -45.F, 1.F});
        CHECK(farClip[2] / farClip[3] == Catch::Approx(0.F).margin(1e-5F));

        // 1 unit ahead along -Z, 1 unit up along +Y with 90 deg vertical FOV -> NDC y = +1.
        const auto topClip = mulMat4Vec4(built->clipFromWorld, {0.F, 1.F, 4.F, 1.F});
        CHECK(topClip[1] / topClip[3] == Catch::Approx(1.F).margin(1e-5F));

        const auto ndcZ = topClip[2] / topClip[3];
        const auto recovered = mulMat4Vec4(built->inverseProjection, {0.F, 1.F, ndcZ, 1.F});
        CHECK(recovered[0] / recovered[3] == Catch::Approx(0.F).margin(1e-5F));
        CHECK(recovered[1] / recovered[3] == Catch::Approx(1.F).margin(1e-5F));
        CHECK(recovered[2] / recovered[3] == Catch::Approx(-1.F).margin(1e-5F));
    }

    SECTION("non-GBuffer view flips projection Y for direct swapchain presentation") {
        const auto built = ar::render::buildFrameView(camera, false);
        REQUIRE(built.has_value());

        const auto topClip = mulMat4Vec4(built->clipFromWorld, {0.F, 1.F, 4.F, 1.F});
        CHECK(topClip[1] / topClip[3] == Catch::Approx(-1.F).margin(1e-5F));
    }

    SECTION("rotated translated camera normalizes quaternion and keeps orthonormal basis") {
        // +90 deg yaw around +Y (unnormalized quaternion {0, 2, 0, 2}).
        ar::render::StaticCamera rotated = camera;
        rotated.position = {2.F, -3.F, 4.F};
        rotated.orientation = {0.F, 2.F, 0.F, 2.F};

        const auto built = ar::render::buildFrameView(rotated, true);
        REQUIRE(built.has_value());

        const auto camInView = mulMat4Vec4(built->view, {2.F, -3.F, 4.F, 1.F});
        CHECK(camInView[0] == Catch::Approx(0.F).margin(1e-5F));
        CHECK(camInView[1] == Catch::Approx(0.F).margin(1e-5F));
        CHECK(camInView[2] == Catch::Approx(0.F).margin(1e-5F));
        CHECK(camInView[3] == Catch::Approx(1.F).margin(1e-5F));

        // +90 deg yaw around +Y rotates local -Z to world -X.
        const auto worldForward = mulUpperMat3Vec3(built->viewToWorld, {0.F, 0.F, -1.F});
        CHECK(worldForward[0] == Catch::Approx(-1.F).margin(1e-5F));
        CHECK(worldForward[1] == Catch::Approx(0.F).margin(1e-5F));
        CHECK(worldForward[2] == Catch::Approx(0.F).margin(1e-5F));

        // 4x4 viewToWorld maps view origin to camera position and satisfies view * viewToWorld == I.
        const auto worldOrigin = mulMat4Vec4(built->viewToWorld, {0.F, 0.F, 0.F, 1.F});
        CHECK(worldOrigin[0] == Catch::Approx(2.F).margin(1e-5F));
        CHECK(worldOrigin[1] == Catch::Approx(-3.F).margin(1e-5F));
        CHECK(worldOrigin[2] == Catch::Approx(4.F).margin(1e-5F));
        CHECK(worldOrigin[3] == Catch::Approx(1.F).margin(1e-5F));

        const auto roundTripPoint =
            mulMat4Vec4(built->viewToWorld, mulMat4Vec4(built->view, {0.F, -3.F, 4.F, 1.F}));
        CHECK(roundTripPoint[0] == Catch::Approx(0.F).margin(1e-5F));
        CHECK(roundTripPoint[1] == Catch::Approx(-3.F).margin(1e-5F));
        CHECK(roundTripPoint[2] == Catch::Approx(4.F).margin(1e-5F));
        CHECK(roundTripPoint[3] == Catch::Approx(1.F).margin(1e-5F));

        // World point 2 units along -X from camera lands at view (0, 0, -2).
        const auto aheadInView = mulMat4Vec4(built->view, {0.F, -3.F, 4.F, 1.F});
        CHECK(aheadInView[0] == Catch::Approx(0.F).margin(1e-5F));
        CHECK(aheadInView[1] == Catch::Approx(0.F).margin(1e-5F));
        CHECK(aheadInView[2] == Catch::Approx(-2.F).margin(1e-5F));

        // viewToWorld * mat3(view) == I for every basis axis.
        const std::array<std::array<float, 3>, 3> axes{
            std::array{1.F, 0.F, 0.F},
            std::array{0.F, 1.F, 0.F},
            std::array{0.F, 0.F, 1.F},
        };
        for (std::size_t axis = 0; axis < axes.size(); ++axis) {
            const auto inView = mulUpperMat3Vec3(built->view, axes[axis]);
            const auto back = mulUpperMat3Vec3(built->viewToWorld, inView);
            for (std::size_t component = 0; component < 3; ++component) {
                CHECK(back[component] == Catch::Approx(axes[axis][component]).margin(1e-5F));
            }
        }
    }

    SECTION("invalid camera parameters are rejected with InvalidArguments") {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();

        const auto expectInvalid = [](const ar::render::StaticCamera& bad) {
            const auto result = ar::render::buildFrameView(bad, true);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().category == ar::core::ErrorCategory::InvalidArguments);
        };

        auto badPosNan = camera;
        badPosNan.position[0] = nan;
        expectInvalid(badPosNan);

        auto badPosInf = camera;
        badPosInf.position[2] = inf;
        expectInvalid(badPosInf);

        auto badQuatZero = camera;
        badQuatZero.orientation = {0.F, 0.F, 0.F, 0.F};
        expectInvalid(badQuatZero);

        auto badQuatUnderflow = camera;
        badQuatUnderflow.orientation = {1e-30F, 0.F, 0.F, 0.F};
        expectInvalid(badQuatUnderflow);

        auto badQuatNan = camera;
        badQuatNan.orientation = {0.F, 0.F, nan, 1.F};
        expectInvalid(badQuatNan);

        auto badNearZero = camera;
        badNearZero.nearPlane = 0.F;
        expectInvalid(badNearZero);

        auto badNearNeg = camera;
        badNearNeg.nearPlane = -0.1F;
        expectInvalid(badNearNeg);

        auto badFarEqual = camera;
        badFarEqual.farPlane = badFarEqual.nearPlane;
        expectInvalid(badFarEqual);

        auto badFarLess = camera;
        badFarLess.farPlane = badFarLess.nearPlane * 0.5F;
        expectInvalid(badFarLess);

        auto badFarInf = camera;
        badFarInf.farPlane = inf;
        expectInvalid(badFarInf);

        auto badFovZero = camera;
        badFovZero.verticalFovRadians = 0.F;
        expectInvalid(badFovZero);

        auto badFovPi = camera;
        badFovPi.verticalFovRadians = std::numbers::pi_v<float>;
        expectInvalid(badFovPi);

        auto badFovNan = camera;
        badFovNan.verticalFovRadians = nan;
        expectInvalid(badFovNan);
    }
}
