#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "platform/Window.h"
#include "platform/WindowInput.h"

TEST_CASE(
    "WindowInputTracker suppresses first-frame, focus, look-edge, and cursor-mode jumps",
    "[platform-input-state]") {
    using ar::platform::CursorMode;
    using ar::platform::KeyInput;
    using ar::platform::WindowInputTracker;

    WindowInputTracker tracker;

    SECTION(
        "first cursor sample establishes baseline with zero delta and subsequent moves "
        "accumulate") {
        tracker.updateCursorPosition(320.0, 180.0);
        auto state = tracker.consume();
        CHECK(state.focused);
        CHECK(state.cursorDeltaX == 0.0);
        CHECK(state.cursorDeltaY == 0.0);

        tracker.updateCursorPosition(335.5, 172.0);
        tracker.updateCursorPosition(340.0, 175.5);
        tracker.addScroll(1.0);
        tracker.addScroll(0.5);
        state = tracker.consume();
        CHECK(state.cursorDeltaX == Catch::Approx(20.0));
        CHECK(state.cursorDeltaY == Catch::Approx(-4.5));
        CHECK(state.scrollDeltaY == Catch::Approx(1.5));

        // Second consume without new motion resets deltas to zero.
        const auto cleared = tracker.consume();
        CHECK(cleared.cursorDeltaX == 0.0);
        CHECK(cleared.cursorDeltaY == 0.0);
        CHECK(cleared.scrollDeltaY == 0.0);
    }

    SECTION("focus loss suppresses inputs and focus regain resets cursor baseline") {
        tracker.updateCursorPosition(100.0, 100.0);
        static_cast<void>(tracker.consume());

        tracker.setFocused(false);
        tracker.updateKeys(KeyInput{true, false, true, false, true, false, true, true});
        tracker.updateLookButton(true);
        tracker.updateCursorPosition(500.0, 500.0);
        tracker.addScroll(3.0);
        const auto unfocused = tracker.consume();
        CHECK_FALSE(unfocused.focused);
        CHECK_FALSE(unfocused.moveForward);
        CHECK_FALSE(unfocused.moveLeft);
        CHECK_FALSE(unfocused.moveUp);
        CHECK_FALSE(unfocused.sprint);
        CHECK_FALSE(unfocused.lookHeld);
        CHECK_FALSE(unfocused.escapePressed);
        CHECK(unfocused.cursorDeltaX == 0.0);
        CHECK(unfocused.cursorDeltaY == 0.0);
        CHECK(unfocused.scrollDeltaY == 0.0);

        // Regaining focus does not jump from the old pre-unfocus cursor position.
        tracker.setFocused(true);
        tracker.updateCursorPosition(700.0, 400.0);
        const auto firstFocused = tracker.consume();
        CHECK(firstFocused.focused);
        CHECK(firstFocused.cursorDeltaX == 0.0);
        CHECK(firstFocused.cursorDeltaY == 0.0);

        tracker.updateCursorPosition(712.0, 395.0);
        const auto secondFocused = tracker.consume();
        CHECK(secondFocused.cursorDeltaX == Catch::Approx(12.0));
        CHECK(secondFocused.cursorDeltaY == Catch::Approx(-5.0));
    }

    SECTION("look button rising edge and cursor mode switch reset cursor baseline") {
        tracker.updateCursorPosition(10.0, 20.0);
        static_cast<void>(tracker.consume());

        // Rising edge of RMB press resets baseline before cursor sample.
        tracker.updateLookButton(true);
        tracker.updateCursorPosition(250.0, 300.0);
        const auto pressFrame = tracker.consume();
        CHECK(pressFrame.lookHeld);
        CHECK(pressFrame.cursorDeltaX == 0.0);
        CHECK(pressFrame.cursorDeltaY == 0.0);

        // Next frame while held emits exact delta.
        tracker.updateLookButton(true);
        tracker.updateCursorPosition(265.0, 290.0);
        const auto dragFrame = tracker.consume();
        CHECK(dragFrame.lookHeld);
        CHECK(dragFrame.cursorDeltaX == Catch::Approx(15.0));
        CHECK(dragFrame.cursorDeltaY == Catch::Approx(-10.0));

        // Switching cursor mode resets baseline to prevent warps.
        tracker.setCursorMode(CursorMode::Disabled);
        CHECK(tracker.cursorMode() == CursorMode::Disabled);
        tracker.updateCursorPosition(640.0, 360.0);
        const auto switchFrame = tracker.consume();
        CHECK(switchFrame.cursorDeltaX == 0.0);
        CHECK(switchFrame.cursorDeltaY == 0.0);

        tracker.updateCursorPosition(648.0, 364.0);
        const auto afterSwitch = tracker.consume();
        CHECK(afterSwitch.cursorDeltaX == Catch::Approx(8.0));
        CHECK(afterSwitch.cursorDeltaY == Catch::Approx(4.0));
    }

    SECTION("escape edge trigger and non-finite sample immunity") {
        tracker.updateKeys(KeyInput{false, false, false, false, false, false, false, true});
        CHECK(tracker.consume().escapePressed);

        // Holding escape does not re-trigger escapePressed on the next frame.
        tracker.updateKeys(KeyInput{false, false, false, false, false, false, false, true});
        CHECK_FALSE(tracker.consume().escapePressed);

        // Releasing and pressing again triggers once.
        tracker.updateKeys(KeyInput{});
        static_cast<void>(tracker.consume());
        tracker.updateKeys(KeyInput{false, false, false, false, false, false, false, true});
        CHECK(tracker.consume().escapePressed);

        tracker.updateCursorPosition(50.0, 50.0);
        static_cast<void>(tracker.consume());
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        tracker.updateCursorPosition(nan, 60.0);
        tracker.updateCursorPosition(60.0, inf);
        tracker.addScroll(nan);
        tracker.addScroll(inf);
        const auto immune = tracker.consume();
        CHECK(immune.cursorDeltaX == 0.0);
        CHECK(immune.cursorDeltaY == 0.0);
        CHECK(immune.scrollDeltaY == 0.0);
    }
}

TEST_CASE(
    "Hidden window GLFW input wiring samples finite state without grabbing cursor",
    "[platform-window-input]") {
    ar::platform::WindowConfig config{};
    config.visible = false;
    auto opened = ar::platform::Window::open(config);
    INFO((opened ? "" : opened.error().message));
    REQUIRE(opened.has_value());
    auto& window = **opened;

    CHECK(window.cursorMode() == ar::platform::CursorMode::Normal);
    window.setCursorMode(ar::platform::CursorMode::Normal);
    CHECK(window.cursorMode() == ar::platform::CursorMode::Normal);

    for (int frame = 0; frame < 3; ++frame) {
        const auto input = window.sampleInput();
        CHECK(std::isfinite(input.cursorDeltaX));
        CHECK(std::isfinite(input.cursorDeltaY));
        CHECK(std::isfinite(input.scrollDeltaY));
        if (frame == 0 || !input.focused) {
            CHECK(input.cursorDeltaX == 0.0);
            CHECK(input.cursorDeltaY == 0.0);
            CHECK(input.scrollDeltaY == 0.0);
        }
    }
}
