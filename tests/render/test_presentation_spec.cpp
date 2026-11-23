#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>

#include "render/PresentationSpec.h"
#include "render/StaticRenderInput.h"

namespace {

template <class T>
void writeAt(std::vector<std::byte>& buffer, std::size_t offset, const T& value) {
    REQUIRE(offset + sizeof(T) <= buffer.size());
    std::memcpy(buffer.data() + offset, &value, sizeof(T));
}

} // namespace

TEST_CASE(
    "render presentation spec defines GBuffer/HDR formats, display push constants, and probe "
    "layout",
    "[render-presentation-spec]") {
    const auto spec = ar::render::buildDeferredPresentationSpec(1.5F);
    CHECK(spec.depthFormat == ar::rhi::AttachmentFormat::D32Sfloat);
    CHECK(spec.depthClear == 0.F);
    CHECK(spec.depthReverseZ);
    CHECK(
        spec.gbufferFormats == std::array{
                                   ar::rhi::AttachmentFormat::Rgba8Unorm,
                                   ar::rhi::AttachmentFormat::Rgba16Sfloat,
                                   ar::rhi::AttachmentFormat::Rg8Unorm});
    CHECK(spec.hdrFormat == ar::rhi::AttachmentFormat::Rgba16Sfloat);
    CHECK(spec.displayExposureEV == Catch::Approx(1.5F));
    REQUIRE(spec.buildDisplayPush != nullptr);

    // Swapchain UNORM -> encodeSrgb = 1; Swapchain SRGB -> encodeSrgb = 0.
    const auto unormPush = spec.buildDisplayPush(spec.displayExposureEV, false);
    REQUIRE(unormPush.size == 8u);
    float exposure = 0.F;
    std::uint32_t encodeSrgb = 0u;
    std::memcpy(&exposure, unormPush.bytes.data(), sizeof(exposure));
    std::memcpy(&encodeSrgb, unormPush.bytes.data() + 4, sizeof(encodeSrgb));
    CHECK(exposure == Catch::Approx(1.5F));
    CHECK(encodeSrgb == 1u);

    const auto srgbPush = spec.buildDisplayPush(-2.25F, true);
    REQUIRE(srgbPush.size == 8u);
    std::memcpy(&exposure, srgbPush.bytes.data(), sizeof(exposure));
    std::memcpy(&encodeSrgb, srgbPush.bytes.data() + 4, sizeof(encodeSrgb));
    CHECK(exposure == Catch::Approx(-2.25F));
    CHECK(encodeSrgb == 0u);

    const auto layout = ar::render::defaultGBufferProbeLayout();
    CHECK(spec.probes.sampleStrideBytes == 32u);
    CHECK(spec.probes.attachmentByteOffsets == std::array<std::uint32_t, 5>{0u, 8u, 16u, 20u, 24u});
    REQUIRE(spec.probes.normalizedCoords.size() == 3u);
    CHECK(spec.probes.normalizedCoords == layout.normalizedCoords);
}

TEST_CASE("halfToFloat decodes IEEE-754 binary16 values accurately", "[render-presentation-spec]") {
    CHECK(ar::render::halfToFloat(0x0000u) == 0.F);
    CHECK(std::signbit(ar::render::halfToFloat(0x8000u)));
    CHECK(ar::render::halfToFloat(0x3C00u) == Catch::Approx(1.F));
    CHECK(ar::render::halfToFloat(0xBC00u) == Catch::Approx(-1.F));
    CHECK(ar::render::halfToFloat(0x3800u) == Catch::Approx(0.5F));
    CHECK(ar::render::halfToFloat(0x4000u) == Catch::Approx(2.F));
    // Smallest positive subnormal: 2^-24.
    CHECK(ar::render::halfToFloat(0x0001u) == Catch::Approx(std::ldexp(1.F, -24)));
    CHECK(std::isinf(ar::render::halfToFloat(0x7C00u)));
    CHECK(ar::render::halfToFloat(0x7C00u) > 0.F);
    CHECK(std::isinf(ar::render::halfToFloat(0xFC00u)));
    CHECK(ar::render::halfToFloat(0xFC00u) < 0.F);
    CHECK(std::isnan(ar::render::halfToFloat(0x7E00u)));
}

TEST_CASE(
    "decodeGBufferProbes decodes attachments and reconstructs Reverse-Z view position",
    "[render-presentation-spec]") {
    ar::rhi::RawProbeCapture raw;
    raw.sampleStrideBytes = 32u;
    raw.attachmentByteOffsets = {0u, 8u, 16u, 20u, 24u};
    raw.pixels = {{160u, 68u}, {16u, 9u}};
    raw.includesHdr = true;
    raw.bytes.resize(64u, std::byte{0});

    // Probe 0: hit at distance 2.5 with near=0.1, far=100, fov=90 deg on 320x180.
    const std::array<std::uint8_t, 4> albedo{51u, 102u, 204u, 255u};
    const std::array<std::uint16_t, 3> normalHalf{0xB800u, 0x0000u, 0x3C00u}; // (-0.5, 0, 1)
    const std::array<std::uint8_t, 2> material{64u, 192u};
    const float distance = 2.5F;
    const float depth = 0.1F * (100.F - distance) / (distance * (100.F - 0.1F));
    const std::array<std::uint16_t, 3> hdrHalf{0x3800u, 0x3C00u, 0x4000u}; // (0.5, 1.0, 2.0)

    writeAt(raw.bytes, 0u, albedo);
    writeAt(raw.bytes, 8u, normalHalf);
    writeAt(raw.bytes, 16u, material);
    writeAt(raw.bytes, 20u, depth);
    writeAt(raw.bytes, 24u, hdrHalf);
    // Probe 1 remains zeroed (background depth == 0).

    ar::render::StaticCamera camera;
    camera.verticalFovRadians = std::numbers::pi_v<float> * 0.5F;
    camera.nearPlane = 0.1F;
    camera.farPlane = 100.F;

    const auto decoded = ar::render::decodeGBufferProbes(raw, 320u, 180u, &camera);
    REQUIRE(decoded.size() == 2u);

    CHECK(decoded[0].pixel == std::array{160u, 68u});
    CHECK(decoded[0].baseColor[0] == Catch::Approx(51.F / 255.F).margin(1e-5F));
    CHECK(decoded[0].baseColor[1] == Catch::Approx(102.F / 255.F).margin(1e-5F));
    CHECK(decoded[0].baseColor[2] == Catch::Approx(204.F / 255.F).margin(1e-5F));
    CHECK(decoded[0].baseColor[3] == Catch::Approx(1.F).margin(1e-5F));
    CHECK(decoded[0].normal[0] == Catch::Approx(-0.5F).margin(1e-5F));
    CHECK(decoded[0].normal[1] == Catch::Approx(0.F).margin(1e-5F));
    CHECK(decoded[0].normal[2] == Catch::Approx(1.F).margin(1e-5F));
    CHECK(decoded[0].material[0] == Catch::Approx(64.F / 255.F).margin(1e-5F));
    CHECK(decoded[0].material[1] == Catch::Approx(192.F / 255.F).margin(1e-5F));
    CHECK(decoded[0].depth == Catch::Approx(depth).margin(1e-6F));
    REQUIRE(decoded[0].hdrColor.has_value());
    CHECK((*decoded[0].hdrColor)[0] == Catch::Approx(0.5F).margin(1e-5F));
    CHECK((*decoded[0].hdrColor)[1] == Catch::Approx(1.F).margin(1e-5F));
    CHECK((*decoded[0].hdrColor)[2] == Catch::Approx(2.F).margin(1e-5F));

    const float expectedX = (2.F * 160.5F / 320.F - 1.F) * distance * (320.F / 180.F);
    const float expectedY = (1.F - 2.F * 68.5F / 180.F) * distance;
    CHECK(decoded[0].viewPosition[0] == Catch::Approx(expectedX).margin(1e-4F));
    CHECK(decoded[0].viewPosition[1] == Catch::Approx(expectedY).margin(1e-4F));
    CHECK(decoded[0].viewPosition[2] == Catch::Approx(-distance).margin(1e-4F));

    CHECK(decoded[1].pixel == std::array{16u, 9u});
    CHECK(decoded[1].depth == 0.F);
    CHECK(decoded[1].viewPosition == std::array{0.F, 0.F, 0.F});

    raw.includesHdr = false;
    const auto withoutHdr = ar::render::decodeGBufferProbes(raw, 320u, 180u, nullptr);
    REQUIRE(withoutHdr.size() == 2u);
    CHECK_FALSE(withoutHdr[0].hdrColor.has_value());
    CHECK(withoutHdr[0].viewPosition == std::array{0.F, 0.F, 0.F});

    // Truncated byte buffer returns empty instead of reading out of bounds.
    raw.bytes.resize(16u);
    CHECK(ar::render::decodeGBufferProbes(raw, 320u, 180u, &camera).empty());
}
