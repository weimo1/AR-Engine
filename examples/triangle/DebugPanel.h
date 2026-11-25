#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "render/StaticRenderInput.h"
#include "rhi/FrameTimings.h"
#include "rhi/Overlay.h"
#include "rhi/PresentationSpec.h"
#include "runtime/RuntimeSession.h"

namespace ar::app {

struct PanelSnapshot {
    std::string sceneRef;
    std::string bindingPath;
    std::string deviceName;
    std::uint32_t instanceCount = 0u;
    double cpuFrameMs = 0.0;
    rhi::GpuFrameTimings gpuTimings{};
    render::StaticCamera activeCamera{};
    float activeExposureEV = 0.F;
    render::DirectionalLight activeLight{};
    bool shadowsSupported = false;
    rhi::DebugViewMode activeViewMode = rhi::DebugViewMode::Final;
    bool cameraLooking = false;
    std::filesystem::path presetDirectory;
};

struct PanelActions {
    bool panelVisible = true;
    bool wantCaptureKeyboard = false;
    bool wantCaptureMouse = false;
    std::optional<float> requestedExposureEV;
    std::optional<render::DirectionalLight> requestedLight;
    std::optional<rhi::DebugViewMode> requestedViewMode;
    bool requestResetCamera = false;
    std::optional<std::filesystem::path> requestSavePreset;
    std::optional<std::filesystem::path> requestLoadPreset;
    rhi::OverlayDrawList drawList;
};

// Owns the Dear ImGui context and widget editing state for ar_engine. No ImGui types leak into
// this header; draw data is converted to RHI's binding-agnostic OverlayDrawList.
class DebugPanel {
public:
    DebugPanel();
    ~DebugPanel();

    DebugPanel(const DebugPanel&) = delete;
    DebugPanel& operator=(const DebugPanel&) = delete;

    [[nodiscard]] rhi::OverlayFontAtlas buildFontAtlas();

    [[nodiscard]] PanelActions tick(
        float deltaSeconds,
        const runtime::FrameInputSnapshot& input,
        const PanelSnapshot& snapshot);

    void setStatus(bool ok, std::string message);
    [[nodiscard]] bool visible() const noexcept;
    void setVisible(bool visible) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ar::app
