#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "core/Error.h"

namespace ar::rhi {

class RhiSession;

struct DescriptorHeapSampleQuery {
    std::uint32_t imageSlot = 0;
    std::uint32_t samplerSlot = 0;
    float u = 0.F;
    float v = 0.F;
};

struct DescriptorHeapProbeResult {
    std::uint64_t resourceHeapAddress = 0;
    std::uint64_t samplerHeapAddress = 0;
    std::vector<std::array<float, 4>> samples;
};

struct DescriptorHeapRetirementResult {
    std::uint32_t retiredImageSlot = 0;
    std::uint64_t retiredImageGeneration = 0;
    std::uint32_t reusedImageSlot = 0;
    std::uint64_t reusedImageGeneration = 0;
    std::uint32_t retiredSamplerSlot = 0;
    std::uint64_t retiredSamplerGeneration = 0;
    std::uint32_t reusedSamplerSlot = 0;
    std::uint64_t reusedSamplerGeneration = 0;

    bool pendingRetireFreedImmediately = false;
    bool pendingImageDestroySucceeded = false;
    bool staleImageHandleRejectedOnRetire = false;
    bool duplicateImageRetireRejected = false;
    bool incompletePublishDeferred = false;
    bool inFlightDestroyRejected = false;
    bool inFlightUploadRejected = false;
    bool inFlightReadbackRejected = false;
    bool inFlightReuseBlocked = false;
    bool staleSamplerHandleRejectedOnRetire = false;
    bool duplicateSamplerRetireRejected = false;
    bool inFlightSamplerReuseBlocked = false;
    bool completedPublishApplied = false;
    bool postCompletionReadbackSucceeded = false;
    bool postCompletionUploadSucceeded = false;
    bool postCompletionDestroySucceeded = false;
    bool staleHandleRejectedAfterReuse = false;
    bool orderedCloseReleasedActiveSlotImage = false;

    std::array<float, 4> inFlightRetiredImageSample{};
    std::array<float, 4> inFlightRetiredSamplerSample{};
    std::array<float, 4> postRetirementDefaultImageSample{};
    std::array<float, 4> postRetirementDefaultSamplerSample{};
    std::array<float, 4> reusedImageSample{};
    std::array<float, 4> reusedSamplerSample{};
};

// Explicit synchronous verification path. Object records use the 32-byte std430 layout.
[[nodiscard]] std::expected<std::array<std::uint32_t, 2>, core::Error> verifyGpuCull(
    RhiSession& session,
    std::span<const std::byte> planes,
    std::span<const std::byte> objects,
    std::span<const std::uint32_t> shader,
    std::span<std::uint32_t> ids,
    std::span<std::array<std::uint32_t, 4>> commands);

// Explicit synchronous Vulkan 1.4 Descriptor Heap verification paths.
[[nodiscard]] std::expected<DescriptorHeapProbeResult, core::Error> verifyDescriptorHeap(
    RhiSession& session,
    std::span<const std::uint32_t> computeShader,
    std::span<const DescriptorHeapSampleQuery> queries);

[[nodiscard]] std::expected<DescriptorHeapRetirementResult, core::Error>
verifyDescriptorHeapRetirement(RhiSession& session, std::span<const std::uint32_t> computeShader);

} // namespace ar::rhi
