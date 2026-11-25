#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ar::rhi {

enum class AttachmentFormat : std::uint32_t {
    Rgba8Unorm,
    Rgba16Sfloat,
    Rg8Unorm,
    D32Sfloat,
};

enum class DebugViewMode : std::uint32_t {
    Final = 0u,
    BaseColor = 1u,
    ViewNormal = 2u,
    MetallicRoughness = 3u,
    MainDepth = 4u,
    ShadowDepth = 5u,
};

inline constexpr std::uint32_t kDebugViewModeCount = 6u;

struct AttachmentProbeLayout {
    std::vector<std::array<float, 2>> normalizedCoords;
    std::uint32_t sampleStrideBytes = 0;
    std::array<std::uint32_t, 5> attachmentByteOffsets{};
};

struct RawProbeCapture {
    std::uint32_t sampleStrideBytes = 0;
    std::array<std::uint32_t, 5> attachmentByteOffsets{};
    std::vector<std::array<std::uint32_t, 2>> pixels;
    std::vector<std::byte> bytes;
    bool includesHdr = false;
};

struct DisplayPushParams {
    float exposureEV = 0.F;
    bool swapchainIsSrgb = false;
    DebugViewMode viewMode = DebugViewMode::Final;
    float cameraNear = 0.1F;
    float cameraFar = 50.F;
};

struct DisplayPushData {
    std::array<std::byte, 24> bytes{};
    std::uint32_t size = 0;
};

using BuildDisplayPush = DisplayPushData (*)(const DisplayPushParams&) noexcept;

struct PresentationSpec {
    std::array<float, 4> clearColor{0.025F, 0.06F, 0.14F, 1.0F};
    AttachmentFormat depthFormat = AttachmentFormat::D32Sfloat;
    float depthClear = 0.F;
    bool depthReverseZ = true;
    std::array<AttachmentFormat, 3> gbufferFormats{
        AttachmentFormat::Rgba8Unorm, AttachmentFormat::Rgba16Sfloat, AttachmentFormat::Rg8Unorm};
    AttachmentFormat hdrFormat = AttachmentFormat::Rgba16Sfloat;
    float displayExposureEV = 0.F;
    BuildDisplayPush buildDisplayPush = nullptr;
    AttachmentProbeLayout probes{};
};

} // namespace ar::rhi
