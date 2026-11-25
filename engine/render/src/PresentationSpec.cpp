#include "render/PresentationSpec.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace ar::render {

rhi::AttachmentProbeLayout defaultGBufferProbeLayout() {
    return rhi::AttachmentProbeLayout{
        {{0.5F, 0.38F}, {0.5F, 0.76F}, {0.05F, 0.05F}},
        32u,
        {0u, 8u, 16u, 20u, 24u},
    };
}

rhi::DisplayPushData packDisplayPushConstants(const rhi::DisplayPushParams& params) noexcept {
    struct DisplayPush {
        float exposureEV;
        std::uint32_t encodeSrgb;
        std::uint32_t viewMode;
        float cameraNear;
        float cameraFar;
    };
    static_assert(sizeof(DisplayPush) == 20u);
    const DisplayPush payload{
        params.exposureEV,
        params.swapchainIsSrgb ? 0u : 1u,
        static_cast<std::uint32_t>(params.viewMode),
        params.cameraNear,
        params.cameraFar};
    rhi::DisplayPushData result{};
    result.size = sizeof(DisplayPush);
    std::memcpy(result.bytes.data(), &payload, sizeof(payload));
    return result;
}

rhi::PresentationSpec buildDeferredPresentationSpec(float exposureEV) {
    rhi::PresentationSpec spec{};
    spec.clearColor = {0.025F, 0.06F, 0.14F, 1.0F};
    spec.depthFormat = rhi::AttachmentFormat::D32Sfloat;
    spec.depthClear = 0.F;
    spec.depthReverseZ = true;
    spec.gbufferFormats = {
        rhi::AttachmentFormat::Rgba8Unorm,
        rhi::AttachmentFormat::Rgba16Sfloat,
        rhi::AttachmentFormat::Rg8Unorm,
    };
    spec.hdrFormat = rhi::AttachmentFormat::Rgba16Sfloat;
    spec.displayExposureEV = exposureEV;
    spec.buildDisplayPush = &packDisplayPushConstants;
    spec.probes = defaultGBufferProbeLayout();
    return spec;
}

float halfToFloat(std::uint16_t bits) noexcept {
    const int exponent = (bits >> 10) & 31;
    const auto fraction = static_cast<float>(bits & 1023);
    float magnitude = 0.F;
    if (exponent == 0) {
        magnitude = std::ldexp(fraction, -24);
    } else if (exponent == 31) {
        magnitude = fraction == 0.F ? std::numeric_limits<float>::infinity()
                                    : std::numeric_limits<float>::quiet_NaN();
    } else {
        magnitude = std::ldexp(1.F + fraction / 1024.F, exponent - 15);
    }
    return (bits & 0x8000u) ? -magnitude : magnitude;
}

std::vector<rhi::GBufferProbe> decodeGBufferProbes(
    const rhi::RawProbeCapture& raw,
    std::uint32_t width,
    std::uint32_t height,
    const StaticCamera* camera) {
    if (raw.pixels.empty() || raw.sampleStrideBytes == 0) {
        return {};
    }
    const std::size_t stride = raw.sampleStrideBytes;
    const auto& offsets = raw.attachmentByteOffsets;
    const auto fits = [&](std::size_t offset, std::size_t size) {
        return offset <= stride && size <= stride - offset;
    };
    if (!fits(offsets[0], 4u) || !fits(offsets[1], 6u) || !fits(offsets[2], 2u) ||
        !fits(offsets[3], sizeof(float)) || (raw.includesHdr && !fits(offsets[4], 6u))) {
        return {};
    }
    if (raw.pixels.size() > raw.bytes.size() / stride) {
        return {};
    }

    const bool reconstructView = camera != nullptr && width > 0 && height > 0;
    const float halfFov = reconstructView ? std::tan(camera->verticalFovRadians * 0.5F) : 0.F;
    const float widthF = static_cast<float>(width);
    const float heightF = static_cast<float>(height);

    std::vector<rhi::GBufferProbe> decoded(raw.pixels.size());
    for (std::size_t i = 0; i < raw.pixels.size(); ++i) {
        auto& output = decoded[i];
        output.pixel = raw.pixels[i];
        const auto* sample = raw.bytes.data() + i * stride;
        for (std::size_t channel = 0; channel < output.baseColor.size(); ++channel) {
            output.baseColor[channel] =
                static_cast<float>(std::to_integer<std::uint8_t>(sample[offsets[0] + channel])) /
                255.F;
        }
        for (std::size_t channel = 0; channel < output.normal.size(); ++channel) {
            std::uint16_t bits = 0;
            std::memcpy(&bits, sample + offsets[1] + channel * sizeof(bits), sizeof(bits));
            output.normal[channel] = halfToFloat(bits);
        }
        for (std::size_t channel = 0; channel < output.material.size(); ++channel) {
            output.material[channel] =
                static_cast<float>(std::to_integer<std::uint8_t>(sample[offsets[2] + channel])) /
                255.F;
        }
        std::memcpy(&output.depth, sample + offsets[3], sizeof(output.depth));
        if (raw.includesHdr) {
            auto& rgb = output.hdrColor.emplace();
            for (std::size_t channel = 0; channel < rgb.size(); ++channel) {
                std::uint16_t bits = 0;
                std::memcpy(&bits, sample + offsets[4] + channel * sizeof(bits), sizeof(bits));
                rgb[channel] = halfToFloat(bits);
            }
        }
        if (reconstructView && output.depth != 0.F) {
            const float distance =
                camera->nearPlane * camera->farPlane /
                (camera->nearPlane + output.depth * (camera->farPlane - camera->nearPlane));
            const float x = 2.F * (static_cast<float>(output.pixel[0]) + 0.5F) / widthF - 1.F;
            const float y = 1.F - 2.F * (static_cast<float>(output.pixel[1]) + 0.5F) / heightF;
            output.viewPosition = {
                x * distance * halfFov * widthF / heightF, y * distance * halfFov, -distance};
        }
    }
    return decoded;
}

} // namespace ar::render
