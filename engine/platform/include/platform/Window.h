#pragma once

#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "core/Error.h"

namespace ar::platform {

struct WindowConfig {
    int width = 1280;
    int height = 720;
    std::string title = "AR Engine";
    bool visible = true;
};

struct FramebufferSize {
    int width = 0;
    int height = 0;
};

// Owns the only active platform window. Call all methods and destroy it on the main thread.
class Window {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<Window>, core::Error>
    open(const WindowConfig& config);

    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void pollEvents() const noexcept;
    void requestClose() noexcept;
    [[nodiscard]] bool shouldClose() const noexcept;
    [[nodiscard]] FramebufferSize framebufferSize() const noexcept;
    [[nodiscard]] bool isMinimized() const noexcept;
    // Borrowed HWND, valid only while this window is alive on the main thread.
    [[nodiscard]] void* nativeWin32Handle() const noexcept;
    [[nodiscard]] std::expected<std::vector<std::string>, core::Error>
    requiredInstanceExtensions() const;

private:
    struct Impl;
    explicit Window(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace ar::platform
