#include "render/FreeCameraController.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace ar::render {
namespace {

bool allFinite(std::span<const float> values) noexcept {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}

core::Error invalidArg(const char* message) {
    return core::Error{
        core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation, message, {}};
}

std::expected<void, core::Error> validateSettings(const FreeCameraSettings& settings) {
    if (!std::isfinite(settings.minMoveSpeed) || !std::isfinite(settings.maxMoveSpeed) ||
        !std::isfinite(settings.moveSpeed) || !(settings.minMoveSpeed > 0.F) ||
        settings.minMoveSpeed > settings.maxMoveSpeed || !(settings.moveSpeed > 0.F)) {
        return std::unexpected(
            invalidArg("free camera speeds must be finite positive with min <= max"));
    }
    if (!std::isfinite(settings.sprintMultiplier) || settings.sprintMultiplier < 1.F) {
        return std::unexpected(invalidArg("free camera sprint multiplier must be >= 1"));
    }
    if (!std::isfinite(settings.lookSensitivityRadiansPerPixel) ||
        settings.lookSensitivityRadiansPerPixel < 0.F) {
        return std::unexpected(invalidArg("free camera look sensitivity must be non-negative"));
    }
    constexpr float kHalfPi = 0.5F * std::numbers::pi_v<float>;
    if (!std::isfinite(settings.maxPitchRadians) || !(settings.maxPitchRadians > 0.F) ||
        !(settings.maxPitchRadians < kHalfPi)) {
        return std::unexpected(invalidArg("free camera maxPitchRadians must be in (0, pi/2)"));
    }
    if (!std::isfinite(settings.maxDeltaSeconds) || !(settings.maxDeltaSeconds > 0.F)) {
        return std::unexpected(invalidArg("free camera maxDeltaSeconds must be positive"));
    }
    return {};
}

struct YawPitch {
    float yaw = 0.F;
    float pitch = 0.F;
};

glm::quat leveledOrientation(float yaw, float pitch) noexcept {
    const glm::quat qYaw = glm::angleAxis(yaw, glm::vec3(0.F, 1.F, 0.F));
    const glm::quat qPitch = glm::angleAxis(pitch, glm::vec3(1.F, 0.F, 0.F));
    return glm::normalize(qYaw * qPitch);
}

std::expected<YawPitch, core::Error>
extractYawPitch(const StaticCamera& camera, float maxPitchRadians) {
    if (!allFinite(camera.position) || !allFinite(camera.orientation)) {
        return std::unexpected(invalidArg("camera position and orientation must be finite"));
    }
    if (!std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) ||
        !(camera.nearPlane > 0.F) || !(camera.farPlane > camera.nearPlane)) {
        return std::unexpected(invalidArg("camera clip planes require 0 < near < far"));
    }
    if (!std::isfinite(camera.verticalFovRadians) || !(camera.verticalFovRadians > 0.F) ||
        !(camera.verticalFovRadians < std::numbers::pi_v<float>)) {
        return std::unexpected(invalidArg("camera vertical FOV must be in (0, pi)"));
    }
    const auto& [x, y, z, w] = camera.orientation;
    const double lengthSquared = static_cast<double>(x) * x + static_cast<double>(y) * y +
                                 static_cast<double>(z) * z + static_cast<double>(w) * w;
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-24) {
        return std::unexpected(invalidArg("camera orientation quaternion must be non-zero"));
    }
    const double invLength = 1.0 / std::sqrt(lengthSquared);
    const glm::quat normalized(
        static_cast<float>(w * invLength),
        static_cast<float>(x * invLength),
        static_cast<float>(y * invLength),
        static_cast<float>(z * invLength));

    const glm::vec3 forward = normalized * glm::vec3(0.F, 0.F, -1.F);
    const float horizontalLenSq = forward.x * forward.x + forward.z * forward.z;
    const float yaw = horizontalLenSq > 1e-10F ? std::atan2(-forward.x, -forward.z) : 0.F;
    const float pitch =
        std::clamp(std::asin(std::clamp(forward.y, -1.F, 1.F)), -maxPitchRadians, maxPitchRadians);
    return YawPitch{yaw, pitch};
}

} // namespace

FreeCameraController::FreeCameraController(
    const StaticCamera& initial,
    const FreeCameraSettings& settings,
    float yaw,
    float pitch) noexcept
    : m_camera(initial), m_settings(settings),
      m_moveSpeed(std::clamp(settings.moveSpeed, settings.minMoveSpeed, settings.maxMoveSpeed)),
      m_yaw(yaw), m_pitch(pitch) {
    const glm::quat q = leveledOrientation(m_yaw, m_pitch);
    m_camera.orientation = {q.x, q.y, q.z, q.w};
}

std::expected<FreeCameraController, core::Error>
FreeCameraController::create(const StaticCamera& initial, const FreeCameraSettings& settings) {
    if (auto valid = validateSettings(settings); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    const auto angles = extractYawPitch(initial, settings.maxPitchRadians);
    if (!angles) {
        return std::unexpected(std::move(angles.error()));
    }
    return FreeCameraController(initial, settings, angles->yaw, angles->pitch);
}

std::expected<void, core::Error> FreeCameraController::reset(const StaticCamera& camera) {
    const auto angles = extractYawPitch(camera, m_settings.maxPitchRadians);
    if (!angles) {
        return std::unexpected(std::move(angles.error()));
    }
    m_camera = camera;
    m_yaw = angles->yaw;
    m_pitch = angles->pitch;
    const glm::quat q = leveledOrientation(m_yaw, m_pitch);
    m_camera.orientation = {q.x, q.y, q.z, q.w};
    return {};
}

StaticCamera
FreeCameraController::update(const FreeCameraInput& input, float deltaSeconds) noexcept {
    const float dt = std::isfinite(deltaSeconds)
                         ? std::clamp(deltaSeconds, 0.F, m_settings.maxDeltaSeconds)
                         : 0.F;

    if (std::isfinite(input.scrollDeltaY) && input.scrollDeltaY != 0.F) {
        const float clampedScroll = std::clamp(input.scrollDeltaY, -40.F, 40.F);
        m_moveSpeed = std::clamp(
            m_moveSpeed * std::exp(clampedScroll * 0.15F),
            m_settings.minMoveSpeed,
            m_settings.maxMoveSpeed);
    }

    if (input.lookActive) {
        if (std::isfinite(input.lookDeltaX)) {
            m_yaw -= input.lookDeltaX * m_settings.lookSensitivityRadiansPerPixel;
        }
        if (std::isfinite(input.lookDeltaY)) {
            m_pitch = std::clamp(
                m_pitch - input.lookDeltaY * m_settings.lookSensitivityRadiansPerPixel,
                -m_settings.maxPitchRadians,
                m_settings.maxPitchRadians);
        }
    }

    const glm::quat orientation = leveledOrientation(m_yaw, m_pitch);
    m_camera.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};

    const glm::vec3 forward = orientation * glm::vec3(0.F, 0.F, -1.F);
    const glm::vec3 right = orientation * glm::vec3(1.F, 0.F, 0.F);
    constexpr glm::vec3 worldUp(0.F, 1.F, 0.F);

    const float forwardAxis =
        static_cast<float>(input.moveForward) - static_cast<float>(input.moveBackward);
    const float rightAxis =
        static_cast<float>(input.moveRight) - static_cast<float>(input.moveLeft);
    const float upAxis = static_cast<float>(input.moveUp) - static_cast<float>(input.moveDown);

    glm::vec3 displacement = forward * forwardAxis + right * rightAxis + worldUp * upAxis;
    const float length = glm::length(displacement);
    if (length > 1e-6F && dt > 0.F) {
        displacement /= length;
        const float speed = m_moveSpeed * (input.sprint ? m_settings.sprintMultiplier : 1.F);
        const glm::vec3 step = displacement * (speed * dt);
        m_camera.position[0] += step.x;
        m_camera.position[1] += step.y;
        m_camera.position[2] += step.z;
    }

    return m_camera;
}

StaticCamera FreeCameraController::camera() const noexcept {
    return m_camera;
}

} // namespace ar::render
