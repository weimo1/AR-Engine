#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Diagnostics.h"
#include "core/Error.h"
#include "platform/Window.h"
#include "platform/WindowInput.h"
#include "render/StaticRenderInput.h"
#include "rhi/Capabilities.h"
#include "rhi/FrameTimings.h"
#include "rhi/Overlay.h"
#include "rhi/PresentationSpec.h"
#include "rhi/RhiSession.h"
#include "runtime/ObservationSettings.h"
#include "runtime/RuntimeConfig.h"

namespace ar::runtime {

enum class FrameStatus { Presented, Paused, Closed };

struct FrameInputSnapshot {
    platform::FramebufferSize windowSize{};
    platform::FramebufferSize framebufferSize{};
    std::vector<platform::UiInputEvent> uiEvents;
    bool focused = true;
    bool minimized = false;
    bool closeRequested = false;
};

// Owned result of a failed start. The caller learns what was established, what was rolled back and
// which diagnostic run ended, without guessing which module exists.
struct StartFailure {
    // Verdict of the failed start. result carries the business outcome, primary_error the earliest
    // cause, cleanup_errors the cleanup failures and diagnostic_errors the output failures.
    core::EndResult endResult;
    // Whatever the query had established. Unexecuted stages stay at their not-queried values.
    rhi::CapabilityReport report;
    // False when the diagnostic session could not be created, so no run directory exists.
    bool diagnosticsAvailable = false;
};

// Owns diagnostics, an optional platform window and one RHI session.
// Single owner thread, not copyable and not movable, so the address stays stable for rhi.
class RuntimeSession {
public:
    static std::expected<std::unique_ptr<RuntimeSession>, StartFailure>
    start(const RuntimeConfig& config);

    ~RuntimeSession();

    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;
    RuntimeSession(RuntimeSession&&) = delete;
    RuntimeSession& operator=(RuntimeSession&&) = delete;

    // Runtime-owned snapshot: readable after close and unchanged by it. The reference dies with
    // the session, so copy before destroying it.
    [[nodiscard]] const rhi::CapabilityReport& report() const noexcept;

    // A paused call waits briefly for window events and does not count as a presented frame.
    // Set pollEvents=false when the caller already pumped events via pollFrameInput() this frame.
    [[nodiscard]] std::expected<FrameStatus, core::Error> presentFrame(
        rhi::FrameScreenshot* screenshot = nullptr,
        const rhi::OverlayDrawList* overlay = nullptr,
        bool pollEvents = true);

    [[nodiscard]] std::expected<void, core::Error>
    openStaticScene(const std::filesystem::path& source, bool verifyRollback = false);
    [[nodiscard]] std::expected<void, core::Error> setCamera(const render::StaticCamera& camera);
    [[nodiscard]] std::optional<render::StaticCamera> activeCamera() const noexcept;
    [[nodiscard]] std::expected<void, core::Error>
    setDirectionalLight(const render::DirectionalLight& light);
    [[nodiscard]] std::optional<render::DirectionalLight> activeDirectionalLight() const noexcept;
    [[nodiscard]] bool shadowsAvailable() const noexcept;
    [[nodiscard]] std::expected<void, core::Error> setExposureEV(float exposureEV);
    [[nodiscard]] float activeExposureEV() const noexcept;
    [[nodiscard]] std::expected<void, core::Error> setDebugView(rhi::DebugViewMode mode);
    [[nodiscard]] rhi::DebugViewMode activeDebugView() const noexcept;
    [[nodiscard]] std::expected<void, core::Error>
    applyObservationSettings(const ObservationSettings& settings);
    [[nodiscard]] std::expected<ObservationSettings, core::Error>
    captureObservationSettings(std::string name, std::string sceneRef) const;
    [[nodiscard]] std::expected<void, core::Error>
    uploadOverlayFont(const rhi::OverlayFontAtlas& atlas);
    [[nodiscard]] rhi::PresentMode activePresentMode() const noexcept;
    [[nodiscard]] rhi::CpuFrameTimings lastCpuTimings() const noexcept;
    [[nodiscard]] rhi::GpuFrameTimings latestGpuTimings() const noexcept;
    [[nodiscard]] std::vector<rhi::GpuFrameTimings> takeCompletedGpuTimings();
    [[nodiscard]] std::expected<std::vector<rhi::GpuFrameTimings>, core::Error>
    drainCompletedGpuTimings();
    [[nodiscard]] std::uint32_t activeInstanceCount() const noexcept;

    [[nodiscard]] std::expected<FrameInputSnapshot, core::Error> pollFrameInput();
    [[nodiscard]] std::expected<void, core::Error>
    setUiCapture(bool captureKeyboard, bool captureMouse);
    [[nodiscard]] std::expected<platform::InputState, core::Error> sampleCameraInput();
    [[nodiscard]] std::expected<platform::InputState, core::Error> sampleWindowInput();
    [[nodiscard]] std::expected<void, core::Error> setCursorMode(platform::CursorMode mode);
    [[nodiscard]] std::optional<platform::CursorMode> cursorMode() const noexcept;
    [[nodiscard]] std::uint64_t staticSceneUploadCount() const noexcept;
    [[nodiscard]] std::expected<void, core::Error>
    saveStaticScene(const std::filesystem::path& destination) const;

    [[nodiscard]] std::expected<void, core::Error>
    replaceEnvironment(const rhi::EnvironmentUpload& upload, bool verifyRollback = false);

    // First call closes rhi, then finishes the diagnostic run. Repeated calls return the cached
    // result without cleaning again or writing a second summary.
    core::EndResult close();

private:
    struct Impl;

    explicit RuntimeSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace ar::runtime
