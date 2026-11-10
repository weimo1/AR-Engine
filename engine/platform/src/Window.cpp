#include "platform/Window.h"

#include <cstdint>
#include <optional>
#include <utility>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

namespace ar::platform {
namespace {

// GLFW lifecycle is process-wide and all platform calls are on the main thread.
bool windowActive = false;

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

} // namespace

struct Window::Impl {
    GLFWwindow* handle = nullptr;
    bool initialized = false;

    ~Impl() {
        if (handle != nullptr) {
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
    return std::unique_ptr<Window>(new Window(std::move(impl)));
}

void Window::pollEvents() const noexcept {
    glfwPollEvents();
}

void Window::requestClose() noexcept {
    glfwSetWindowShouldClose(m_impl->handle, GLFW_TRUE);
}

bool Window::shouldClose() const noexcept {
    return glfwWindowShouldClose(m_impl->handle) == GLFW_TRUE;
}

FramebufferSize Window::framebufferSize() const noexcept {
    FramebufferSize size;
    glfwGetFramebufferSize(m_impl->handle, &size.width, &size.height);
    return size;
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
