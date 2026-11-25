#include "platform/Window.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

namespace ar::platform {
namespace {

// GLFW lifecycle is process-wide and all platform calls are on the main thread.
bool windowActive = false;
constexpr std::size_t kMaxQueuedUiEvents = 1024u;

core::Error glfwError(core::ErrorCategory category, core::ErrorStage stage, std::string message) {
    const char* description = nullptr;
    const int code = glfwGetError(&description);
    if (description != nullptr) {
        message += ": ";
        message += description;
    }
    core::Error error{category, stage, std::move(message), std::nullopt};
    if (code != GLFW_NO_ERROR) {
        error.origin = core::ErrorOrigin{"internal", code};
    }
    return error;
}

KeyCode mapGlfwKey(int key) noexcept {
    switch (key) {
    case GLFW_KEY_TAB:
        return KeyCode::Tab;
    case GLFW_KEY_LEFT:
        return KeyCode::Left;
    case GLFW_KEY_RIGHT:
        return KeyCode::Right;
    case GLFW_KEY_UP:
        return KeyCode::Up;
    case GLFW_KEY_DOWN:
        return KeyCode::Down;
    case GLFW_KEY_PAGE_UP:
        return KeyCode::PageUp;
    case GLFW_KEY_PAGE_DOWN:
        return KeyCode::PageDown;
    case GLFW_KEY_HOME:
        return KeyCode::Home;
    case GLFW_KEY_END:
        return KeyCode::End;
    case GLFW_KEY_INSERT:
        return KeyCode::Insert;
    case GLFW_KEY_DELETE:
        return KeyCode::Delete;
    case GLFW_KEY_BACKSPACE:
        return KeyCode::Backspace;
    case GLFW_KEY_SPACE:
        return KeyCode::Space;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
        return KeyCode::Enter;
    case GLFW_KEY_ESCAPE:
        return KeyCode::Escape;
    case GLFW_KEY_LEFT_CONTROL:
        return KeyCode::LeftCtrl;
    case GLFW_KEY_RIGHT_CONTROL:
        return KeyCode::RightCtrl;
    case GLFW_KEY_LEFT_SHIFT:
        return KeyCode::LeftShift;
    case GLFW_KEY_RIGHT_SHIFT:
        return KeyCode::RightShift;
    case GLFW_KEY_LEFT_ALT:
        return KeyCode::LeftAlt;
    case GLFW_KEY_RIGHT_ALT:
        return KeyCode::RightAlt;
    case GLFW_KEY_A:
        return KeyCode::A;
    case GLFW_KEY_C:
        return KeyCode::C;
    case GLFW_KEY_D:
        return KeyCode::D;
    case GLFW_KEY_E:
        return KeyCode::E;
    case GLFW_KEY_Q:
        return KeyCode::Q;
    case GLFW_KEY_S:
        return KeyCode::S;
    case GLFW_KEY_V:
        return KeyCode::V;
    case GLFW_KEY_W:
        return KeyCode::W;
    case GLFW_KEY_X:
        return KeyCode::X;
    case GLFW_KEY_Y:
        return KeyCode::Y;
    case GLFW_KEY_Z:
        return KeyCode::Z;
    case GLFW_KEY_F1:
        return KeyCode::F1;
    case GLFW_KEY_F2:
        return KeyCode::F2;
    case GLFW_KEY_F3:
        return KeyCode::F3;
    case GLFW_KEY_F4:
        return KeyCode::F4;
    case GLFW_KEY_F5:
        return KeyCode::F5;
    case GLFW_KEY_F6:
        return KeyCode::F6;
    case GLFW_KEY_0:
        return KeyCode::Digit0;
    case GLFW_KEY_1:
        return KeyCode::Digit1;
    case GLFW_KEY_2:
        return KeyCode::Digit2;
    case GLFW_KEY_3:
        return KeyCode::Digit3;
    case GLFW_KEY_4:
        return KeyCode::Digit4;
    case GLFW_KEY_5:
        return KeyCode::Digit5;
    case GLFW_KEY_6:
        return KeyCode::Digit6;
    default:
        return KeyCode::Unknown;
    }
}

std::optional<MouseButton> mapGlfwMouseButton(int button) noexcept {
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT:
        return MouseButton::Left;
    case GLFW_MOUSE_BUTTON_RIGHT:
        return MouseButton::Right;
    case GLFW_MOUSE_BUTTON_MIDDLE:
        return MouseButton::Middle;
    default:
        return std::nullopt;
    }
}

} // namespace

struct Window::Impl {
    GLFWwindow* handle = nullptr;
    bool initialized = false;
    WindowInputTracker tracker{};
    std::vector<UiInputEvent> uiEvents;

    void pushUiEvent(const UiInputEvent& event) noexcept {
        if (uiEvents.size() >= kMaxQueuedUiEvents) {
            return;
        }
        try {
            uiEvents.push_back(event);
        } catch (...) {
            // GLFW C callbacks must not unwind across C frames.
        }
    }

    static void
    handleKey(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) noexcept {
        if (action != GLFW_PRESS && action != GLFW_RELEASE) {
            return;
        }
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window))) {
            const KeyCode mapped = mapGlfwKey(key);
            if (mapped != KeyCode::Unknown) {
                UiInputEvent event{};
                event.kind = UiInputEvent::Kind::Key;
                event.key = mapped;
                event.pressed = (action == GLFW_PRESS);
                impl->pushUiEvent(event);
            }
        }
    }

    static void handleChar(GLFWwindow* window, unsigned int codepoint) noexcept {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window))) {
            UiInputEvent event{};
            event.kind = UiInputEvent::Kind::Char;
            event.codepoint = static_cast<std::uint32_t>(codepoint);
            impl->pushUiEvent(event);
        }
    }

    static void
    handleMouseButton(GLFWwindow* window, int button, int action, int /*mods*/) noexcept {
        if (action != GLFW_PRESS && action != GLFW_RELEASE) {
            return;
        }
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window))) {
            if (const auto mapped = mapGlfwMouseButton(button)) {
                UiInputEvent event{};
                event.kind = UiInputEvent::Kind::MouseButton;
                event.mouseButton = *mapped;
                event.pressed = (action == GLFW_PRESS);
                impl->pushUiEvent(event);
            }
        }
    }

    static void handleCursorPos(GLFWwindow* window, double x, double y) noexcept {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window))) {
            UiInputEvent event{};
            event.kind = UiInputEvent::Kind::MouseMove;
            event.x = x;
            event.y = y;
            impl->pushUiEvent(event);
        }
    }

    static void handleScroll(GLFWwindow* window, double xOffset, double yOffset) noexcept {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window))) {
            impl->tracker.addScroll(yOffset);
            UiInputEvent event{};
            event.kind = UiInputEvent::Kind::MouseScroll;
            event.x = xOffset;
            event.y = yOffset;
            impl->pushUiEvent(event);
        }
    }

    static void handleFocus(GLFWwindow* window, int focused) noexcept {
        if (auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window))) {
            const bool isFocused = (focused == GLFW_TRUE);
            impl->tracker.setFocused(isFocused);
            UiInputEvent event{};
            event.kind = UiInputEvent::Kind::Focus;
            event.pressed = isFocused;
            impl->pushUiEvent(event);
        }
    }

    ~Impl() {
        if (handle != nullptr) {
            glfwSetKeyCallback(handle, nullptr);
            glfwSetCharCallback(handle, nullptr);
            glfwSetMouseButtonCallback(handle, nullptr);
            glfwSetCursorPosCallback(handle, nullptr);
            glfwSetScrollCallback(handle, nullptr);
            glfwSetWindowFocusCallback(handle, nullptr);
            glfwSetWindowUserPointer(handle, nullptr);
            glfwDestroyWindow(handle);
        }
        if (initialized) {
            glfwTerminate();
            windowActive = false;
        }
    }
};

Window::Window(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
Window::~Window() = default;

std::expected<std::unique_ptr<Window>, core::Error> Window::open(const WindowConfig& config) {
    if (config.width <= 0 || config.height <= 0) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Validation,
                "Window dimensions must be positive",
                std::nullopt});
    }
    if (windowActive) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Validation,
                "A platform window is already active",
                std::nullopt});
    }

    auto impl = std::make_unique<Impl>();
    if (glfwInit() != GLFW_TRUE) {
        return std::unexpected(glfwError(
            core::ErrorCategory::InitializationFailed,
            core::ErrorStage::Validation,
            "GLFW initialization failed"));
    }
    impl->initialized = true;
    windowActive = true;
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);
    impl->handle =
        glfwCreateWindow(config.width, config.height, config.title.c_str(), nullptr, nullptr);
    if (impl->handle == nullptr) {
        return std::unexpected(glfwError(
            core::ErrorCategory::InitializationFailed,
            core::ErrorStage::Validation,
            "GLFW window creation failed"));
    }
    impl->tracker.setFocused(glfwGetWindowAttrib(impl->handle, GLFW_FOCUSED) == GLFW_TRUE);
    glfwSetWindowUserPointer(impl->handle, impl.get());
    glfwSetKeyCallback(impl->handle, Window::Impl::handleKey);
    glfwSetCharCallback(impl->handle, Window::Impl::handleChar);
    glfwSetMouseButtonCallback(impl->handle, Window::Impl::handleMouseButton);
    glfwSetCursorPosCallback(impl->handle, Window::Impl::handleCursorPos);
    glfwSetScrollCallback(impl->handle, Window::Impl::handleScroll);
    glfwSetWindowFocusCallback(impl->handle, Window::Impl::handleFocus);
    return std::unique_ptr<Window>(new Window(std::move(impl)));
}

void Window::pollEvents() const noexcept {
    glfwPollEvents();
}

void Window::waitEvents(double seconds) const noexcept {
    glfwWaitEventsTimeout(seconds);
}

std::vector<UiInputEvent> Window::takeUiEvents() noexcept {
    std::vector<UiInputEvent> events;
    events.swap(m_impl->uiEvents);
    return events;
}

void Window::setUiCapture(bool captureKeyboard, bool captureMouse) noexcept {
    m_impl->tracker.setUiCapture(captureKeyboard, captureMouse);
}

InputState Window::sampleInput(bool poll) noexcept {
    if (poll) {
        glfwPollEvents();
    }
    const bool focused = glfwGetWindowAttrib(m_impl->handle, GLFW_FOCUSED) == GLFW_TRUE;
    m_impl->tracker.setFocused(focused);
    const auto keyDown = [this](int key) noexcept {
        return glfwGetKey(m_impl->handle, key) == GLFW_PRESS;
    };
    m_impl->tracker.updateKeys(
        KeyInput{
            keyDown(GLFW_KEY_W),
            keyDown(GLFW_KEY_S),
            keyDown(GLFW_KEY_A),
            keyDown(GLFW_KEY_D),
            keyDown(GLFW_KEY_E),
            keyDown(GLFW_KEY_Q),
            keyDown(GLFW_KEY_LEFT_SHIFT) || keyDown(GLFW_KEY_RIGHT_SHIFT),
            keyDown(GLFW_KEY_ESCAPE),
        });
    m_impl->tracker.updateLookButton(
        glfwGetMouseButton(m_impl->handle, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
    double cursorX = 0.0;
    double cursorY = 0.0;
    glfwGetCursorPos(m_impl->handle, &cursorX, &cursorY);
    m_impl->tracker.updateCursorPosition(cursorX, cursorY);
    return m_impl->tracker.consume();
}

void Window::setCursorMode(CursorMode mode) noexcept {
    const int glfwMode = mode == CursorMode::Disabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL;
    glfwSetInputMode(m_impl->handle, GLFW_CURSOR, glfwMode);
    m_impl->tracker.setCursorMode(mode);
}

CursorMode Window::cursorMode() const noexcept {
    return m_impl->tracker.cursorMode();
}

void Window::requestClose() noexcept {
    glfwSetWindowShouldClose(m_impl->handle, GLFW_TRUE);
}

bool Window::shouldClose() const noexcept {
    return glfwWindowShouldClose(m_impl->handle) == GLFW_TRUE;
}

FramebufferSize Window::windowSize() const noexcept {
    FramebufferSize size;
    glfwGetWindowSize(m_impl->handle, &size.width, &size.height);
    return size;
}

FramebufferSize Window::framebufferSize() const noexcept {
    FramebufferSize size;
    glfwGetFramebufferSize(m_impl->handle, &size.width, &size.height);
    return size;
}

bool Window::isVisible() const noexcept {
    return glfwGetWindowAttrib(m_impl->handle, GLFW_VISIBLE) == GLFW_TRUE;
}

bool Window::isMinimized() const noexcept {
    return glfwGetWindowAttrib(m_impl->handle, GLFW_ICONIFIED) == GLFW_TRUE;
}

void* Window::nativeWin32Handle() const noexcept {
    return glfwGetWin32Window(m_impl->handle);
}

std::expected<std::vector<std::string>, core::Error> Window::requiredInstanceExtensions() const {
    std::uint32_t count = 0;
    glfwGetError(nullptr);
    const char** names = glfwGetRequiredInstanceExtensions(&count);
    if (names == nullptr || count == 0) {
        return std::unexpected(glfwError(
            core::ErrorCategory::QueryFailed,
            core::ErrorStage::Capability,
            "GLFW could not provide Vulkan instance extensions"));
    }
    return std::vector<std::string>(names, names + count);
}

} // namespace ar::platform
