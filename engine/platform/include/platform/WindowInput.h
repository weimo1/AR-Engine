#pragma once

#include <cstdint>

namespace ar::platform {

enum class CursorMode { Normal, Disabled };

enum class KeyCode : std::uint16_t {
    Unknown = 0,
    Tab,
    Left,
    Right,
    Up,
    Down,
    PageUp,
    PageDown,
    Home,
    End,
    Insert,
    Delete,
    Backspace,
    Space,
    Enter,
    Escape,
    LeftCtrl,
    RightCtrl,
    LeftShift,
    RightShift,
    LeftAlt,
    RightAlt,
    A,
    C,
    D,
    E,
    Q,
    S,
    V,
    W,
    X,
    Y,
    Z,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
};

enum class MouseButton : std::uint8_t {
    Left = 0,
    Right = 1,
    Middle = 2,
};

struct UiInputEvent {
    enum class Kind : std::uint8_t {
        Key,
        Char,
        MouseMove,
        MouseButton,
        MouseScroll,
        Focus,
    };

    Kind kind = Kind::Focus;
    KeyCode key = KeyCode::Unknown;
    MouseButton mouseButton = MouseButton::Left;
    bool pressed = false;
    std::uint32_t codepoint = 0u;
    double x = 0.0;
    double y = 0.0;

    bool operator==(const UiInputEvent&) const = default;
};

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

// Pure CPU input state machine that filters focus transitions, UI capture gates, cursor mode
// switches, right-button look edges, and first-frame cursor baselines without OS dependencies.
class WindowInputTracker {
public:
    void setFocused(bool focused) noexcept;
    void setCursorMode(CursorMode mode) noexcept;
    void setUiCapture(bool captureKeyboard, bool captureMouse) noexcept;
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
    [[nodiscard]] bool uiCaptureKeyboard() const noexcept {
        return m_uiCaptureKeyboard;
    }
    [[nodiscard]] bool uiCaptureMouse() const noexcept {
        return m_uiCaptureMouse;
    }

private:
    bool m_focused = true;
    bool m_uiCaptureKeyboard = false;
    bool m_uiCaptureMouse = false;
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
