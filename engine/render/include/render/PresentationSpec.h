#pragma once

#include <cstdint>
#include <vector>

#include "render/StaticRenderInput.h"
#include "rhi/PresentationSpec.h"
#include "rhi/RhiSession.h"

namespace ar::render {

[[nodiscard]] rhi::AttachmentProbeLayout defaultGBufferProbeLayout();
[[nodiscard]] rhi::DisplayPushData
packDisplayPushConstants(const rhi::DisplayPushParams& params) noexcept;
[[nodiscard]] rhi::PresentationSpec buildDeferredPresentationSpec(float exposureEV = 0.F);
[[nodiscard]] float halfToFloat(std::uint16_t bits) noexcept;
[[nodiscard]] std::vector<rhi::GBufferProbe> decodeGBufferProbes(
    const rhi::RawProbeCapture& raw,
    std::uint32_t width,
    std::uint32_t height,
    const StaticCamera* camera = nullptr);

} // namespace ar::render
