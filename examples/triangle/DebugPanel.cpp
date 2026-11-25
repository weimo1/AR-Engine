#include "DebugPanel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>

#include <imgui.h>

namespace ar::app {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDegToRad = kPi / 180.F;
constexpr float kRadToDeg = 180.F / kPi;

ImGuiKey mapKeyCode(platform::KeyCode key) noexcept {
    using platform::KeyCode;
    switch (key) {
    case KeyCode::Tab:
        return ImGuiKey_Tab;
    case KeyCode::Left:
        return ImGuiKey_LeftArrow;
    case KeyCode::Right:
        return ImGuiKey_RightArrow;
    case KeyCode::Up:
        return ImGuiKey_UpArrow;
    case KeyCode::Down:
        return ImGuiKey_DownArrow;
    case KeyCode::PageUp:
        return ImGuiKey_PageUp;
    case KeyCode::PageDown:
        return ImGuiKey_PageDown;
    case KeyCode::Home:
        return ImGuiKey_Home;
    case KeyCode::End:
        return ImGuiKey_End;
    case KeyCode::Insert:
        return ImGuiKey_Insert;
    case KeyCode::Delete:
        return ImGuiKey_Delete;
    case KeyCode::Backspace:
        return ImGuiKey_Backspace;
    case KeyCode::Space:
        return ImGuiKey_Space;
    case KeyCode::Enter:
        return ImGuiKey_Enter;
    case KeyCode::Escape:
        return ImGuiKey_Escape;
    case KeyCode::LeftCtrl:
        return ImGuiKey_LeftCtrl;
    case KeyCode::RightCtrl:
        return ImGuiKey_RightCtrl;
    case KeyCode::LeftShift:
        return ImGuiKey_LeftShift;
    case KeyCode::RightShift:
        return ImGuiKey_RightShift;
    case KeyCode::LeftAlt:
        return ImGuiKey_LeftAlt;
    case KeyCode::RightAlt:
        return ImGuiKey_RightAlt;
    case KeyCode::A:
        return ImGuiKey_A;
    case KeyCode::C:
        return ImGuiKey_C;
    case KeyCode::D:
        return ImGuiKey_D;
    case KeyCode::E:
        return ImGuiKey_E;
    case KeyCode::Q:
        return ImGuiKey_Q;
    case KeyCode::S:
        return ImGuiKey_S;
    case KeyCode::V:
        return ImGuiKey_V;
    case KeyCode::W:
        return ImGuiKey_W;
    case KeyCode::X:
        return ImGuiKey_X;
    case KeyCode::Y:
        return ImGuiKey_Y;
    case KeyCode::Z:
        return ImGuiKey_Z;
    case KeyCode::F1:
        return ImGuiKey_F1;
    case KeyCode::F2:
        return ImGuiKey_F2;
    case KeyCode::F3:
        return ImGuiKey_F3;
    case KeyCode::F4:
        return ImGuiKey_F4;
    case KeyCode::F5:
        return ImGuiKey_F5;
    case KeyCode::F6:
        return ImGuiKey_F6;
    case KeyCode::Digit0:
        return ImGuiKey_0;
    case KeyCode::Digit1:
        return ImGuiKey_1;
    case KeyCode::Digit2:
        return ImGuiKey_2;
    case KeyCode::Digit3:
        return ImGuiKey_3;
    case KeyCode::Digit4:
        return ImGuiKey_4;
    case KeyCode::Digit5:
        return ImGuiKey_5;
    case KeyCode::Digit6:
        return ImGuiKey_6;
    case KeyCode::Unknown:
        return ImGuiKey_None;
    }
    return ImGuiKey_None;
}

void directionToAngles(
    const std::array<float, 3>& dir, float& azimuthDeg, float& elevationDeg) noexcept {
    // Light propagates along dir; source direction is -dir.
    const float sx = -dir[0];
    const float sy = -dir[1];
    const float sz = -dir[2];
    const float len = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (len <= 1e-6F) {
        azimuthDeg = 35.F;
        elevationDeg = 55.F;
        return;
    }
    const float nx = sx / len;
    const float ny = std::clamp(sy / len, -1.F, 1.F);
    const float nz = sz / len;
    elevationDeg = std::asin(ny) * kRadToDeg;
    azimuthDeg = std::atan2(nx, nz) * kRadToDeg;
}

std::array<float, 3> anglesToDirection(float azimuthDeg, float elevationDeg) noexcept {
    const float az = azimuthDeg * kDegToRad;
    const float el = std::clamp(elevationDeg, 5.F, 89.F) * kDegToRad;
    const float cosEl = std::cos(el);
    const float sx = std::sin(az) * cosEl;
    const float sy = std::sin(el);
    const float sz = std::cos(az) * cosEl;
    return {-sx, -sy, -sz};
}

rhi::OverlayDrawList convertDrawData(const ImDrawData* drawData) {
    rhi::OverlayDrawList out{};
    if (!drawData || drawData->CmdListsCount <= 0 || drawData->TotalVtxCount <= 0 ||
        drawData->TotalIdxCount <= 0) {
        return out;
    }
    const float fbWidth = drawData->DisplaySize.x * drawData->FramebufferScale.x;
    const float fbHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;
    if (fbWidth <= 0.F || fbHeight <= 0.F) {
        return out;
    }
    out.displayWidth = drawData->DisplaySize.x;
    out.displayHeight = drawData->DisplaySize.y;
    out.vertices.reserve(static_cast<std::size_t>(drawData->TotalVtxCount));
    out.indices.reserve(static_cast<std::size_t>(drawData->TotalIdxCount));

    const ImVec2 clipOff = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;
    static_assert(sizeof(ImDrawVert) == sizeof(rhi::OverlayVertex));
    static_assert(sizeof(ImDrawIdx) == sizeof(std::uint16_t));

    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
        const ImDrawList* cmdList = drawData->CmdLists[listIndex];
        const auto baseVertex = static_cast<std::int32_t>(out.vertices.size());
        const auto baseIndex = static_cast<std::uint32_t>(out.indices.size());

        const std::size_t prevVerts = out.vertices.size();
        out.vertices.resize(prevVerts + static_cast<std::size_t>(cmdList->VtxBuffer.Size));
        std::memcpy(
            out.vertices.data() + prevVerts,
            cmdList->VtxBuffer.Data,
            static_cast<std::size_t>(cmdList->VtxBuffer.Size) * sizeof(rhi::OverlayVertex));

        const std::size_t prevIndices = out.indices.size();
        out.indices.resize(prevIndices + static_cast<std::size_t>(cmdList->IdxBuffer.Size));
        std::memcpy(
            out.indices.data() + prevIndices,
            cmdList->IdxBuffer.Data,
            static_cast<std::size_t>(cmdList->IdxBuffer.Size) * sizeof(std::uint16_t));

        for (int cmdIndex = 0; cmdIndex < cmdList->CmdBuffer.Size; ++cmdIndex) {
            const ImDrawCmd& pcmd = cmdList->CmdBuffer[cmdIndex];
            if (pcmd.UserCallback != nullptr || pcmd.ElemCount == 0u) {
                continue;
            }
            const float x0 = std::max(0.F, (pcmd.ClipRect.x - clipOff.x) * clipScale.x);
            const float y0 = std::max(0.F, (pcmd.ClipRect.y - clipOff.y) * clipScale.y);
            const float x1 = std::min(fbWidth, (pcmd.ClipRect.z - clipOff.x) * clipScale.x);
            const float y1 = std::min(fbHeight, (pcmd.ClipRect.w - clipOff.y) * clipScale.y);
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            rhi::OverlayDrawCommand cmd{};
            cmd.indexOffset = baseIndex + pcmd.IdxOffset;
            cmd.indexCount = pcmd.ElemCount;
            cmd.vertexOffset = baseVertex + static_cast<std::int32_t>(pcmd.VtxOffset);
            cmd.clipX = static_cast<std::int32_t>(x0);
            cmd.clipY = static_cast<std::int32_t>(y0);
            cmd.clipWidth = static_cast<std::uint32_t>(x1 - x0);
            cmd.clipHeight = static_cast<std::uint32_t>(y1 - y0);
            out.commands.push_back(cmd);
        }
    }
    return out;
}

} // namespace

struct DebugPanel::Impl {
    ImGuiContext* context = nullptr;
    bool visible = true;
    bool modCtrlLeft = false;
    bool modCtrlRight = false;
    bool modShiftLeft = false;
    bool modShiftRight = false;
    bool modAltLeft = false;
    bool modAltRight = false;

    std::array<char, 128> presetFilename{};
    bool statusOk = true;
    std::string statusMessage;

    Impl() {
        IMGUI_CHECKVERSION();
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 6.F;
        style.FrameRounding = 4.F;
        style.GrabRounding = 4.F;
        style.Colors[ImGuiCol_WindowBg].w = 0.86F;
        std::snprintf(presetFilename.data(), presetFilename.size(), "observation_preset.json");
    }

    ~Impl() {
        if (context) {
            ImGui::SetCurrentContext(context);
            ImGui::DestroyContext(context);
            context = nullptr;
        }
    }
};

DebugPanel::DebugPanel() : m_impl(std::make_unique<Impl>()) {}
DebugPanel::~DebugPanel() = default;

rhi::OverlayFontAtlas DebugPanel::buildFontAtlas() {
    ImGui::SetCurrentContext(m_impl->context);
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    rhi::OverlayFontAtlas atlas{};
    if (pixels != nullptr && width > 0 && height > 0) {
        atlas.width = static_cast<std::uint32_t>(width);
        atlas.height = static_cast<std::uint32_t>(height);
        const std::size_t byteCount =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
        atlas.rgba.assign(pixels, pixels + byteCount);
    }
    return atlas;
}

PanelActions DebugPanel::tick(
    float deltaSeconds, const runtime::FrameInputSnapshot& input, const PanelSnapshot& snapshot) {
    ImGui::SetCurrentContext(m_impl->context);
    ImGuiIO& io = ImGui::GetIO();

    // Forward platform events to ImGui and handle F1 panel toggle.
    PanelActions actions{};
    for (const auto& event : input.uiEvents) {
        switch (event.kind) {
        case platform::UiInputEvent::Kind::Key: {
            if (event.key == platform::KeyCode::F1 && event.pressed) {
                m_impl->visible = !m_impl->visible;
            }
            if (event.key == platform::KeyCode::LeftCtrl) {
                m_impl->modCtrlLeft = event.pressed;
            } else if (event.key == platform::KeyCode::RightCtrl) {
                m_impl->modCtrlRight = event.pressed;
            } else if (event.key == platform::KeyCode::LeftShift) {
                m_impl->modShiftLeft = event.pressed;
            } else if (event.key == platform::KeyCode::RightShift) {
                m_impl->modShiftRight = event.pressed;
            } else if (event.key == platform::KeyCode::LeftAlt) {
                m_impl->modAltLeft = event.pressed;
            } else if (event.key == platform::KeyCode::RightAlt) {
                m_impl->modAltRight = event.pressed;
            }
            io.AddKeyEvent(ImGuiMod_Ctrl, m_impl->modCtrlLeft || m_impl->modCtrlRight);
            io.AddKeyEvent(ImGuiMod_Shift, m_impl->modShiftLeft || m_impl->modShiftRight);
            io.AddKeyEvent(ImGuiMod_Alt, m_impl->modAltLeft || m_impl->modAltRight);
            const ImGuiKey mapped = mapKeyCode(event.key);
            if (mapped != ImGuiKey_None) {
                io.AddKeyEvent(mapped, event.pressed);
            }
            break;
        }
        case platform::UiInputEvent::Kind::Char:
            io.AddInputCharacter(event.codepoint);
            break;
        case platform::UiInputEvent::Kind::MouseMove:
            if (!snapshot.cameraLooking) {
                io.AddMousePosEvent(static_cast<float>(event.x), static_cast<float>(event.y));
            }
            break;
        case platform::UiInputEvent::Kind::MouseButton:
            if (!snapshot.cameraLooking) {
                io.AddMouseButtonEvent(static_cast<int>(event.mouseButton), event.pressed);
            }
            break;
        case platform::UiInputEvent::Kind::MouseScroll:
            if (!snapshot.cameraLooking) {
                io.AddMouseWheelEvent(static_cast<float>(event.x), static_cast<float>(event.y));
            }
            break;
        case platform::UiInputEvent::Kind::Focus:
            io.AddFocusEvent(event.pressed);
            break;
        }
    }
    if (snapshot.cameraLooking) {
        const float offscreen = -std::numeric_limits<float>::max();
        io.AddMousePosEvent(offscreen, offscreen);
    }

    actions.panelVisible = m_impl->visible;
    if (!m_impl->visible || input.windowSize.width <= 0 || input.windowSize.height <= 0 ||
        input.framebufferSize.width <= 0 || input.framebufferSize.height <= 0) {
        return actions;
    }

    io.DisplaySize = ImVec2(
        static_cast<float>(input.windowSize.width), static_cast<float>(input.windowSize.height));
    io.DisplayFramebufferScale = ImVec2(
        static_cast<float>(input.framebufferSize.width) /
            static_cast<float>(input.windowSize.width),
        static_cast<float>(input.framebufferSize.height) /
            static_cast<float>(input.windowSize.height));
    io.DeltaTime = std::clamp(deltaSeconds, 1.0F / 1000.F, 0.1F);

    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(14.F, 14.F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.F, 0.F), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("AR Engine Inspector  [F1]", &m_impl->visible)) {
        ImGui::Text("Device: %s", snapshot.deviceName.c_str());
        ImGui::Text("Binding: %s", snapshot.bindingPath.c_str());
        ImGui::Text("Scene: %s (%u instances)", snapshot.sceneRef.c_str(), snapshot.instanceCount);

        ImGui::SeparatorText("Frame Timings");
        ImGui::Text("CPU Frame: %.2f ms", snapshot.cpuFrameMs);
        if (snapshot.gpuTimings.available && snapshot.gpuTimings.totalGpuMs) {
            ImGui::Text(
                "GPU Total (#%llu): %.3f ms",
                static_cast<unsigned long long>(snapshot.gpuTimings.frameNumber),
                *snapshot.gpuTimings.totalGpuMs);
            if (snapshot.gpuTimings.shadowMs) {
                ImGui::Text("  Shadow:   %.3f ms", *snapshot.gpuTimings.shadowMs);
            }
            if (snapshot.gpuTimings.cullMs) {
                ImGui::Text("  Cull:     %.3f ms", *snapshot.gpuTimings.cullMs);
            }
            if (snapshot.gpuTimings.geometryMs) {
                ImGui::Text("  Geometry: %.3f ms", *snapshot.gpuTimings.geometryMs);
            }
            if (snapshot.gpuTimings.lightingMs) {
                ImGui::Text("  Lighting: %.3f ms", *snapshot.gpuTimings.lightingMs);
            }
            if (snapshot.gpuTimings.displayMs) {
                ImGui::Text("  Display:  %.3f ms", *snapshot.gpuTimings.displayMs);
            }
            if (snapshot.gpuTimings.overlayMs) {
                ImGui::Text("  Overlay:  %.3f ms", *snapshot.gpuTimings.overlayMs);
            }
        } else {
            ImGui::TextDisabled(
                "GPU: unavailable (%s)",
                snapshot.gpuTimings.unavailableReason.empty()
                    ? "pending"
                    : snapshot.gpuTimings.unavailableReason.c_str());
        }

        ImGui::SeparatorText("Debug View (6 Channels)");
        constexpr std::array<const char*, rhi::kDebugViewModeCount> kViewLabels{
            "Final (ACES HDR)",
            "BaseColor (Albedo)",
            "View Normal",
            "Metallic / Roughness",
            "Main Depth (Reverse-Z)",
            "Shadow Depth (1:1)",
        };
        const bool shadowViewAvailable =
            snapshot.shadowsSupported && snapshot.activeLight.castShadows;
        int currentMode = static_cast<int>(snapshot.activeViewMode);
        for (int i = 0; i < static_cast<int>(kViewLabels.size()); ++i) {
            const bool isShadowView = (i == static_cast<int>(rhi::DebugViewMode::ShadowDepth));
            const bool disabled = isShadowView && !shadowViewAvailable;
            if (disabled) {
                ImGui::BeginDisabled();
            }
            const char* label =
                disabled ? "Shadow Depth (Unavailable: Shadows Off)" : kViewLabels[i];
            if (ImGui::RadioButton(label, currentMode == i)) {
                actions.requestedViewMode = static_cast<rhi::DebugViewMode>(i);
            }
            if (disabled) {
                ImGui::EndDisabled();
            }
        }

        ImGui::SeparatorText("Exposure & Directional Light");
        float exposure = snapshot.activeExposureEV;
        if (ImGui::SliderFloat("Exposure (EV)", &exposure, -4.0F, 4.0F, "%.2f")) {
            actions.requestedExposureEV = exposure;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("0")) {
            actions.requestedExposureEV = 0.F;
        }

        render::DirectionalLight light = snapshot.activeLight;
        bool lightChanged = false;
        if (snapshot.shadowsSupported) {
            if (ImGui::Checkbox("Cast Directional Shadows", &light.castShadows)) {
                lightChanged = true;
            }
        } else {
            ImGui::TextDisabled("Directional Shadows: Disabled at startup");
        }
        if (ImGui::SliderFloat("Intensity", &light.intensity, 0.0F, 12.0F, "%.2f")) {
            lightChanged = true;
        }
        float azimuth = 0.F;
        float elevation = 0.F;
        directionToAngles(light.direction, azimuth, elevation);
        bool anglesChanged = false;
        anglesChanged |= ImGui::SliderFloat("Light Azimuth", &azimuth, -180.F, 180.F, "%.1f deg");
        anglesChanged |= ImGui::SliderFloat("Light Elevation", &elevation, 8.F, 88.F, "%.1f deg");
        if (anglesChanged) {
            light.direction = anglesToDirection(azimuth, elevation);
            lightChanged = true;
        }
        if (ImGui::ColorEdit3("Light Color", light.color.data())) {
            lightChanged = true;
        }
        if (lightChanged) {
            actions.requestedLight = light;
        }

        ImGui::SeparatorText("Camera");
        ImGui::Text(
            "Pos: (%.2f, %.2f, %.2f)",
            snapshot.activeCamera.position[0],
            snapshot.activeCamera.position[1],
            snapshot.activeCamera.position[2]);
        if (ImGui::Button("Reset Camera")) {
            actions.requestResetCamera = true;
        }
        ImGui::TextDisabled("RMB: Look | WASD/QE: Move | Shift: Fast | Wheel: Speed");

        ImGui::SeparatorText("Observation Preset");
        ImGui::InputText("File", m_impl->presetFilename.data(), m_impl->presetFilename.size());
        const std::filesystem::path presetPath =
            snapshot.presetDirectory / std::filesystem::path(m_impl->presetFilename.data());
        if (ImGui::Button("Save Preset")) {
            actions.requestSavePreset = presetPath;
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Preset")) {
            actions.requestLoadPreset = presetPath;
        }
        if (!m_impl->statusMessage.empty()) {
            const ImVec4 color = m_impl->statusOk ? ImVec4(0.45F, 0.92F, 0.55F, 1.F)
                                                  : ImVec4(0.96F, 0.42F, 0.42F, 1.F);
            ImGui::TextColored(color, "%s", m_impl->statusMessage.c_str());
        }
    }
    ImGui::End();

    ImGui::Render();
    actions.panelVisible = m_impl->visible;
    actions.wantCaptureKeyboard =
        !snapshot.cameraLooking && m_impl->visible && io.WantCaptureKeyboard;
    actions.wantCaptureMouse = !snapshot.cameraLooking && m_impl->visible && io.WantCaptureMouse;
    actions.drawList = convertDrawData(ImGui::GetDrawData());
    return actions;
}

void DebugPanel::setStatus(bool ok, std::string message) {
    m_impl->statusOk = ok;
    m_impl->statusMessage = std::move(message);
}

bool DebugPanel::visible() const noexcept {
    return m_impl->visible;
}

void DebugPanel::setVisible(bool visible) noexcept {
    m_impl->visible = visible;
}

} // namespace ar::app
