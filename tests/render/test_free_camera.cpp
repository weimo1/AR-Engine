#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include "render/FreeCameraController.h"

namespace {

std::array<float, 3> rotateByQuat(const std::array<float, 4>& q, const std::array<float, 3>& v) {
    const float qx = q[0];
    const float qy = q[1];
    const float qz = q[2];
    const float qw = q[3];
    const float tx = 2.F * (qy * v[2] - qz * v[1]);
    const float ty = 2.F * (qz * v[0] - qx * v[2]);
    const float tz = 2.F * (qx * v[1] - qy * v[0]);
    return {
        v[0] + qw * tx + qy * tz - qz * ty,
        v[1] + qw * ty + qz * tx - qx * tz,
        v[2] + qw * tz + qx * ty - qy * tx,
    };
}

std::array<float, 4> mulQuat(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
}

std::array<float, 4> axisAngleQuat(const std::array<float, 3>& axis, float radians) {
    const float half = 0.5F * radians;
    const float s = std::sin(half);
    return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(half)};
}

float absQuatDot(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
}

} // namespace

TEST_CASE(
    "FreeCameraController enforces zero-roll takeover, movement, look clamping, and dt guards",
    "[render-free-camera]") {
    ar::render::StaticCamera base{};
    base.position = {1.F, 2.F, 3.F};
    base.orientation = {0.F, 0.F, 0.F, 1.F};

    SECTION("zero-roll initial quaternions round-trip identically") {
        const std::array<std::pair<float, float>, 4> angles{{
            {0.F, 0.F},
            {0.75F, -0.35F},
            {-2.1F, 0.85F},
            {std::numbers::pi_v<float>, -0.2F},
        }};
        for (const auto& [yaw, pitch] : angles) {
            ar::render::StaticCamera cam = base;
            const auto qYaw = axisAngleQuat({0.F, 1.F, 0.F}, yaw);
            const auto qPitch = axisAngleQuat({1.F, 0.F, 0.F}, pitch);
            cam.orientation = mulQuat(qYaw, qPitch);

            const auto controller = ar::render::FreeCameraController::create(cam);
            REQUIRE(controller.has_value());
            const auto out = controller->camera();
            CHECK(out.position == cam.position);
            CHECK(absQuatDot(out.orientation, cam.orientation) == Catch::Approx(1.F).margin(1e-5F));

            const auto expectedFwd = rotateByQuat(cam.orientation, {0.F, 0.F, -1.F});
            const auto actualFwd = rotateByQuat(out.orientation, {0.F, 0.F, -1.F});
            for (std::size_t i = 0; i < 3u; ++i) {
                CHECK(actualFwd[i] == Catch::Approx(expectedFwd[i]).margin(1e-5F));
            }
        }
    }

    SECTION(
        "rolled initial quaternions converge on first takeover to zero roll preserving forward") {
        const auto qYaw = axisAngleQuat({0.F, 1.F, 0.F}, 0.65F);
        const auto qPitch = axisAngleQuat({1.F, 0.F, 0.F}, -0.30F);
        const auto qRoll = axisAngleQuat({0.F, 0.F, -1.F}, 0.61F); // ~35 deg roll
        ar::render::StaticCamera rolled = base;
        rolled.orientation = mulQuat(mulQuat(qYaw, qPitch), qRoll);

        const auto rolledRight = rotateByQuat(rolled.orientation, {1.F, 0.F, 0.F});
        REQUIRE(std::abs(rolledRight[1]) > 0.4F);

        auto controller = ar::render::FreeCameraController::create(rolled);
        REQUIRE(controller.has_value());
        const auto leveled = controller->camera();

        // Forward direction matches original rolled camera exactly.
        const auto expectedFwd = rotateByQuat(rolled.orientation, {0.F, 0.F, -1.F});
        const auto actualFwd = rotateByQuat(leveled.orientation, {0.F, 0.F, -1.F});
        for (std::size_t i = 0; i < 3u; ++i) {
            CHECK(actualFwd[i] == Catch::Approx(expectedFwd[i]).margin(1e-5F));
        }

        // Right axis is strictly horizontal (zero roll).
        const auto leveledRight = rotateByQuat(leveled.orientation, {1.F, 0.F, 0.F});
        CHECK(leveledRight[1] == Catch::Approx(0.F).margin(1e-5F));

        // Reset also converges cleanly back to zero-roll pose.
        REQUIRE(controller->reset(rolled).has_value());
        CHECK(controller->camera().orientation == leveled.orientation);
    }

    SECTION("WASD/QE movement, diagonal normalization, sprint, and dt clamping") {
        ar::render::FreeCameraSettings settings{};
        settings.moveSpeed = 4.0F;
        settings.sprintMultiplier = 2.5F;
        settings.maxDeltaSeconds = 0.1F;

        auto controller = ar::render::FreeCameraController::create(base, settings);
        REQUIRE(controller.has_value());

        // Forward (-Z) by 0.05s -> delta Z = -0.2.
        ar::render::FreeCameraInput forward{};
        forward.moveForward = true;
        auto cam = controller->update(forward, 0.05F);
        CHECK(cam.position[0] == Catch::Approx(1.F));
        CHECK(cam.position[1] == Catch::Approx(2.F));
        CHECK(cam.position[2] == Catch::Approx(2.8F));

        // Diagonal forward + right normalized so step length equals speed * dt.
        REQUIRE(controller->reset(base).has_value());
        ar::render::FreeCameraInput diag{};
        diag.moveForward = true;
        diag.moveRight = true;
        cam = controller->update(diag, 0.05F);
        const float dx = cam.position[0] - base.position[0];
        const float dy = cam.position[1] - base.position[1];
        const float dz = cam.position[2] - base.position[2];
        CHECK(dy == Catch::Approx(0.F));
        CHECK(dx > 0.F);
        CHECK(dz < 0.F);
        CHECK(std::sqrt(dx * dx + dz * dz) == Catch::Approx(4.0F * 0.05F).margin(1e-5F));

        // Up/Down + sprint + dt clamped to maxDeltaSeconds (0.1s).
        REQUIRE(controller->reset(base).has_value());
        ar::render::FreeCameraInput upSprint{};
        upSprint.moveUp = true;
        upSprint.sprint = true;
        cam = controller->update(upSprint, 5.0F);
        CHECK(cam.position[1] == Catch::Approx(2.F + 4.0F * 2.5F * 0.1F).margin(1e-5F));

        // Negative / NaN / Inf dt produce zero movement.
        const auto posBefore = cam.position;
        CHECK(controller->update(upSprint, -1.0F).position == posBefore);
        CHECK(
            controller->update(upSprint, std::numeric_limits<float>::quiet_NaN()).position ==
            posBefore);
        CHECK(
            controller->update(upSprint, std::numeric_limits<float>::infinity()).position ==
            posBefore);
    }

    SECTION(
        "look drag direction, inactive look suppression, pitch clamp, and scroll speed limits") {
        ar::render::FreeCameraSettings settings{};
        settings.moveSpeed = 4.0F;
        settings.minMoveSpeed = 0.5F;
        settings.maxMoveSpeed = 20.0F;
        settings.lookSensitivityRadiansPerPixel = 0.01F;

        auto controller = ar::render::FreeCameraController::create(base, settings);
        REQUIRE(controller.has_value());

        // Mouse delta ignored when lookActive is false.
        ar::render::FreeCameraInput noLook{};
        noLook.lookActive = false;
        noLook.lookDeltaX = 100.F;
        noLook.lookDeltaY = -50.F;
        controller->update(noLook, 0.016F);
        CHECK(controller->yawRadians() == Catch::Approx(0.F));
        CHECK(controller->pitchRadians() == Catch::Approx(0.F));

        // Right drag (+dx) turns right (+X forward); upward drag (-dy) tilts up (+Y forward).
        ar::render::FreeCameraInput lookRightUp{};
        lookRightUp.lookActive = true;
        lookRightUp.lookDeltaX = 50.F;
        lookRightUp.lookDeltaY = -25.F;
        const auto turned = controller->update(lookRightUp, 0.016F);
        const auto fwd = rotateByQuat(turned.orientation, {0.F, 0.F, -1.F});
        CHECK(fwd[0] > 0.3F);
        CHECK(fwd[1] > 0.2F);

        // Extreme pitch clamps to +/- maxPitchRadians (~89 deg).
        ar::render::FreeCameraInput hugeUp{};
        hugeUp.lookActive = true;
        hugeUp.lookDeltaY = -10000.F;
        controller->update(hugeUp, 0.016F);
        CHECK(controller->pitchRadians() == Catch::Approx(settings.maxPitchRadians));

        ar::render::FreeCameraInput hugeDown{};
        hugeDown.lookActive = true;
        hugeDown.lookDeltaY = 10000.F;
        controller->update(hugeDown, 0.016F);
        CHECK(controller->pitchRadians() == Catch::Approx(-settings.maxPitchRadians));

        // Scroll scales moveSpeed within [minMoveSpeed, maxMoveSpeed] and ignores NaN.
        ar::render::FreeCameraInput scrollUp{};
        scrollUp.scrollDeltaY = 100.F;
        controller->update(scrollUp, 0.016F);
        CHECK(controller->moveSpeed() == Catch::Approx(settings.maxMoveSpeed));

        ar::render::FreeCameraInput scrollDown{};
        scrollDown.scrollDeltaY = -100.F;
        controller->update(scrollDown, 0.016F);
        CHECK(controller->moveSpeed() == Catch::Approx(settings.minMoveSpeed));

        const float speedBeforeNan = controller->moveSpeed();
        ar::render::FreeCameraInput scrollNan{};
        scrollNan.scrollDeltaY = std::numeric_limits<float>::quiet_NaN();
        controller->update(scrollNan, 0.016F);
        CHECK(controller->moveSpeed() == Catch::Approx(speedBeforeNan));
    }

    SECTION("invalid camera and settings are rejected") {
        ar::render::StaticCamera badCam = base;
        badCam.orientation = {0.F, 0.F, 0.F, 0.F};
        CHECK_FALSE(ar::render::FreeCameraController::create(badCam).has_value());

        ar::render::FreeCameraSettings badSettings{};
        badSettings.minMoveSpeed = 10.F;
        badSettings.maxMoveSpeed = 5.F;
        CHECK_FALSE(ar::render::FreeCameraController::create(base, badSettings).has_value());

        auto valid = ar::render::FreeCameraController::create(base);
        REQUIRE(valid.has_value());
        CHECK_FALSE(valid->reset(badCam).has_value());
    }
}
