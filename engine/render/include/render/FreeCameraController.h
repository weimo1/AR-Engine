#pragma once

#include <expected>

#include "core/Error.h"
#include "render/StaticRenderInput.h"

namespace ar::render {

struct FreeCameraInput {
    bool moveForward = false;
    bool moveBackward = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveUp = false;
    bool moveDown = false;
    bool sprint = false;
    bool lookActive = false;
    float lookDeltaX = 0.F;
    float lookDeltaY = 0.F;
    float scrollDeltaY = 0.F;
};

struct FreeCameraSettings {
    float moveSpeed = 3.5F;
    float minMoveSpeed = 0.25F;
    float maxMoveSpeed = 50.0F;
    float sprintMultiplier = 2.5F;
    float lookSensitivityRadiansPerPixel = 0.0025F;
    float maxPitchRadians = 1.55334303F; // 89 degrees
    float maxDeltaSeconds = 0.1F;
};

// Pure CPU zero-roll free camera controller. Accepts any valid non-zero camera quaternion:
// zero-roll inputs round-trip identically; rolled inputs converge on takeover to a zero-roll
// orientation preserving the primary forward (yaw, pitch) with a horizontal right axis.
class FreeCameraController {
public:
    [[nodiscard]] static std::expected<FreeCameraController, core::Error>
    create(const StaticCamera& initial, const FreeCameraSettings& settings = {});

    std::expected<void, core::Error> reset(const StaticCamera& camera);
    StaticCamera update(const FreeCameraInput& input, float deltaSeconds) noexcept;

    [[nodiscard]] StaticCamera camera() const noexcept;
    [[nodiscard]] float moveSpeed() const noexcept {
        return m_moveSpeed;
    }
    [[nodiscard]] float yawRadians() const noexcept {
        return m_yaw;
    }
    [[nodiscard]] float pitchRadians() const noexcept {
        return m_pitch;
    }

private:
    FreeCameraController(
        const StaticCamera& initial,
        const FreeCameraSettings& settings,
        float yaw,
        float pitch) noexcept;

    StaticCamera m_camera{};
    FreeCameraSettings m_settings{};
    float m_moveSpeed = 3.5F;
    float m_yaw = 0.F;
    float m_pitch = 0.F;
};

} // namespace ar::render
