#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "platform/Window.h"

namespace {

using ar::platform::Window;
using ar::platform::WindowConfig;

TEST_CASE("a hidden window reports its state and can be recreated", "[platform-window-create]") {
    WindowConfig config;
    config.visible = false;

    auto invalid = config;
    invalid.width = 0;
    const auto rejected = Window::open(invalid);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().category == ar::core::ErrorCategory::InvalidArguments);

    auto opened = Window::open(config);
    INFO((opened ? "" : opened.error().message));
    REQUIRE(opened.has_value());
    auto& window = **opened;

    const auto duplicate = Window::open(config);
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().category == ar::core::ErrorCategory::InvalidState);

    for (int frame = 0; frame < 3; ++frame) {
        window.pollEvents();
    }
    const auto size = window.framebufferSize();
    CHECK(size.width > 0);
    CHECK(size.height > 0);
    CHECK_FALSE(window.isMinimized());
    CHECK_FALSE(window.shouldClose());
    window.requestClose();
    CHECK(window.shouldClose());

    opened->reset();
    REQUIRE(Window::open(config).has_value());
}

TEST_CASE("Vulkan extension names outlive their window", "[platform-window-vulkan-extensions]") {
    WindowConfig config;
    config.visible = false;
    auto opened = Window::open(config);
    INFO((opened ? "" : opened.error().message));
    REQUIRE(opened.has_value());

    const auto extensions = (*opened)->requiredInstanceExtensions();
    INFO((extensions ? "" : extensions.error().message));
    REQUIRE(extensions.has_value());
    opened->reset();

    CHECK(std::ranges::find(*extensions, "VK_KHR_surface") != extensions->end());
}

} // namespace
