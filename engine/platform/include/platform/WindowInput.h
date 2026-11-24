#pragma once

namespace ar::platform {

enum class CursorMode { Normal, Disabled };

struct KeyInput {
    bool moveForward = false;
    bool moveBackward = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveUp = false;
    bool moveDown = false;
    bool sprint = false;
    bool escape = false;

    bool operator==(const KeyInput&) const = default;
};

struct InputState {
    bool focused = true;
    bool moveForward = false;
    bool moveBackward = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveUp = false;
    bool moveDown = false;
    bool sprint = false;
    bool lookHeld = false;
    bool escapePressed = false;
    double cursorDeltaX = 0.0;
    double cursorDeltaY = 0.0;
    double scrollDeltaY = 0.0;

    bool operator==(const InputState&) const = default;
};

// Pure CPU input state machine that filters focus transitions, cursor mode switches,
// right-button look edges, and first-frame cursor baselines without OS or GLFW dependencies.
class WindowInputTracker {
public:
    void setFocused(bool focused) noexcept;
    void setCursorMode(CursorMode mode) noexcept;
    void updateKeys(const KeyInput& keys) noexcept;
    void updateLookButton(bool held) noexcept;
    void updateCursorPosition(double x, double y) noexcept;
    void addScroll(double deltaY) noexcept;

    [[nodiscard]] InputState consume() noexcept;
    [[nodiscard]] CursorMode cursorMode() const noexcept {
        return m_cursorMode;
    }
    [[nodiscard]] bool focused() const noexcept {
        return m_focused;
    }

private:
    bool m_focused = true;
    CursorMode m_cursorMode = CursorMode::Normal;
    KeyInput m_keys{};
    bool m_prevEscape = false;
    bool m_escapePressed = false;
    bool m_lookHeld = false;
    bool m_hasLastCursor = false;
    double m_lastCursorX = 0.0;
    double m_lastCursorY = 0.0;
    double m_cursorDeltaX = 0.0;
    double m_cursorDeltaY = 0.0;
    double m_scrollDeltaY = 0.0;
};

} // namespace ar::platform
