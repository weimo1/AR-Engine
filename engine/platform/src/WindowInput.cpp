#include "platform/WindowInput.h"

#include <cmath>

namespace ar::platform {

void WindowInputTracker::setFocused(bool focused) noexcept {
    if (m_focused == focused) {
        return;
    }
    m_focused = focused;
    m_hasLastCursor = false;
    m_cursorDeltaX = 0.0;
    m_cursorDeltaY = 0.0;
    if (!m_focused) {
        m_keys = {};
        m_prevEscape = false;
        m_escapePressed = false;
        m_lookHeld = false;
        m_scrollDeltaY = 0.0;
    }
}

void WindowInputTracker::setCursorMode(CursorMode mode) noexcept {
    if (m_cursorMode == mode) {
        return;
    }
    m_cursorMode = mode;
    m_hasLastCursor = false;
    m_cursorDeltaX = 0.0;
    m_cursorDeltaY = 0.0;
}

void WindowInputTracker::updateKeys(const KeyInput& keys) noexcept {
    if (!m_focused) {
        m_keys = {};
        m_prevEscape = false;
        m_escapePressed = false;
        return;
    }
    m_keys = keys;
    if (keys.escape && !m_prevEscape) {
        m_escapePressed = true;
    }
    m_prevEscape = keys.escape;
}

void WindowInputTracker::updateLookButton(bool held) noexcept {
    if (!m_focused) {
        m_lookHeld = false;
        m_hasLastCursor = false;
        m_cursorDeltaX = 0.0;
        m_cursorDeltaY = 0.0;
        return;
    }
    if (held && !m_lookHeld) {
        // Reset baseline on the rising edge so the initial look-press frame never jumps.
        m_hasLastCursor = false;
        m_cursorDeltaX = 0.0;
        m_cursorDeltaY = 0.0;
    }
    m_lookHeld = held;
}

void WindowInputTracker::updateCursorPosition(double x, double y) noexcept {
    if (!m_focused || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    if (!m_hasLastCursor) {
        m_lastCursorX = x;
        m_lastCursorY = y;
        m_hasLastCursor = true;
        return;
    }
    m_cursorDeltaX += x - m_lastCursorX;
    m_cursorDeltaY += y - m_lastCursorY;
    m_lastCursorX = x;
    m_lastCursorY = y;
}

void WindowInputTracker::addScroll(double deltaY) noexcept {
    if (!m_focused || !std::isfinite(deltaY)) {
        return;
    }
    m_scrollDeltaY += deltaY;
}

InputState WindowInputTracker::consume() noexcept {
    InputState state{};
    state.focused = m_focused;
    if (m_focused) {
        state.moveForward = m_keys.moveForward;
        state.moveBackward = m_keys.moveBackward;
        state.moveLeft = m_keys.moveLeft;
        state.moveRight = m_keys.moveRight;
        state.moveUp = m_keys.moveUp;
        state.moveDown = m_keys.moveDown;
        state.sprint = m_keys.sprint;
        state.lookHeld = m_lookHeld;
        state.escapePressed = m_escapePressed;
        state.cursorDeltaX = m_cursorDeltaX;
        state.cursorDeltaY = m_cursorDeltaY;
        state.scrollDeltaY = m_scrollDeltaY;
    }
    m_escapePressed = false;
    m_cursorDeltaX = 0.0;
    m_cursorDeltaY = 0.0;
    m_scrollDeltaY = 0.0;
    return state;
}

} // namespace ar::platform
