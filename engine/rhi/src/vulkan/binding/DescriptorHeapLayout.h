#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>

#include "rhi/RhiSession.h"

namespace ar::rhi::detail {

struct DescriptorHeapCapacity {
    std::uint32_t imageCount = 0;
    std::uint32_t bufferCount = 0;
    std::uint32_t samplerCount = 0;
};

struct DescriptorHeapPlan {
    std::uint64_t imageStride = 0;
    std::uint64_t imageRegionSize = 0;
    std::uint64_t bufferBaseOffset = 0;
    std::uint64_t bufferStride = 0;
    std::uint64_t bufferRegionSize = 0;
    std::uint64_t resourceReservedOffset = 0;
    std::uint64_t resourceReservedSize = 0;
    std::uint64_t resourceHeapSize = 0;
    std::uint64_t samplerStride = 0;
    std::uint64_t samplerRegionSize = 0;
    std::uint64_t samplerReservedOffset = 0;
    std::uint64_t samplerReservedSize = 0;
    std::uint64_t samplerHeapSize = 0;
};

enum class DescriptorHeapLayoutError {
    InvalidLimits,
    InvalidCapacity,
    Overflow,
    ResourceHeapExceeded,
    SamplerHeapExceeded
};

[[nodiscard]] constexpr bool isPowerOfTwo(std::uint64_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
checkedAlignUp(std::uint64_t value, std::uint64_t alignment) noexcept {
    if (!isPowerOfTwo(alignment)) {
        return std::nullopt;
    }
    const std::uint64_t mask = alignment - 1;
    if (value > (std::numeric_limits<std::uint64_t>::max)() - mask) {
        return std::nullopt;
    }
    return (value + mask) & ~mask;
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
checkedMultiply(std::uint64_t count, std::uint64_t stride) noexcept {
    if (count != 0 && stride > (std::numeric_limits<std::uint64_t>::max)() / count) {
        return std::nullopt;
    }
    return count * stride;
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
checkedAdd(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    if (lhs > (std::numeric_limits<std::uint64_t>::max)() - rhs) {
        return std::nullopt;
    }
    return lhs + rhs;
}

[[nodiscard]] constexpr bool
validateDescriptorHeapLimits(const DescriptorHeapLimits& limits) noexcept {
    return isPowerOfTwo(limits.samplerHeapAlignment) &&
           isPowerOfTwo(limits.resourceHeapAlignment) &&
           isPowerOfTwo(limits.samplerDescriptorAlignment) &&
           isPowerOfTwo(limits.imageDescriptorAlignment) &&
           isPowerOfTwo(limits.bufferDescriptorAlignment) && limits.samplerDescriptorSize > 0 &&
           limits.imageDescriptorSize > 0 && limits.bufferDescriptorSize > 0 &&
           limits.maxPushDataSize > 0 &&
           limits.maxSamplerHeapSize > limits.minSamplerHeapReservedRange &&
           limits.maxResourceHeapSize > limits.minResourceHeapReservedRange;
}

[[nodiscard]] constexpr std::expected<DescriptorHeapPlan, DescriptorHeapLayoutError>
planDescriptorHeaps(const DescriptorHeapLimits& limits, DescriptorHeapCapacity capacity) noexcept {
    if (!validateDescriptorHeapLimits(limits)) {
        return std::unexpected(DescriptorHeapLayoutError::InvalidLimits);
    }
    if (capacity.imageCount == 0 || capacity.samplerCount == 0) {
        return std::unexpected(DescriptorHeapLayoutError::InvalidCapacity);
    }

    const auto imageStride =
        checkedAlignUp(limits.imageDescriptorSize, limits.imageDescriptorAlignment);
    const auto bufferStride =
        checkedAlignUp(limits.bufferDescriptorSize, limits.bufferDescriptorAlignment);
    const auto samplerStride =
        checkedAlignUp(limits.samplerDescriptorSize, limits.samplerDescriptorAlignment);
    if (!imageStride || !bufferStride || !samplerStride) {
        return std::unexpected(DescriptorHeapLayoutError::Overflow);
    }

    const auto imageRegionSize = checkedMultiply(capacity.imageCount, *imageStride);
    const auto bufferRegionSize = checkedMultiply(capacity.bufferCount, *bufferStride);
    const auto samplerRegionSize = checkedMultiply(capacity.samplerCount, *samplerStride);
    if (!imageRegionSize || !bufferRegionSize || !samplerRegionSize) {
        return std::unexpected(DescriptorHeapLayoutError::Overflow);
    }

    const auto bufferBaseOffset =
        capacity.bufferCount > 0
            ? checkedAlignUp(*imageRegionSize, limits.bufferDescriptorAlignment)
            : std::optional<std::uint64_t>{*imageRegionSize};
    if (!bufferBaseOffset) {
        return std::unexpected(DescriptorHeapLayoutError::Overflow);
    }

    const auto resourceUsedEnd = checkedAdd(*bufferBaseOffset, *bufferRegionSize);
    if (!resourceUsedEnd) {
        return std::unexpected(DescriptorHeapLayoutError::Overflow);
    }

    const std::uint64_t resourceReservedAlignment =
        (std::
             max)({limits.resourceHeapAlignment, limits.imageDescriptorAlignment, limits.bufferDescriptorAlignment});
    const std::uint64_t samplerReservedAlignment =
        (std::max)(limits.samplerHeapAlignment, limits.samplerDescriptorAlignment);
    const auto resourceReservedOffset = checkedAlignUp(*resourceUsedEnd, resourceReservedAlignment);
    const auto samplerReservedOffset = checkedAlignUp(*samplerRegionSize, samplerReservedAlignment);
    if (!resourceReservedOffset || !samplerReservedOffset) {
        return std::unexpected(DescriptorHeapLayoutError::Overflow);
    }

    const auto resourceHeapSize =
        checkedAdd(*resourceReservedOffset, limits.minResourceHeapReservedRange);
    const auto samplerHeapSize =
        checkedAdd(*samplerReservedOffset, limits.minSamplerHeapReservedRange);
    if (!resourceHeapSize || !samplerHeapSize) {
        return std::unexpected(DescriptorHeapLayoutError::Overflow);
    }

    if (*resourceHeapSize > limits.maxResourceHeapSize) {
        return std::unexpected(DescriptorHeapLayoutError::ResourceHeapExceeded);
    }
    if (*samplerHeapSize > limits.maxSamplerHeapSize) {
        return std::unexpected(DescriptorHeapLayoutError::SamplerHeapExceeded);
    }

    return DescriptorHeapPlan{
        *imageStride,
        *imageRegionSize,
        *bufferBaseOffset,
        *bufferStride,
        *bufferRegionSize,
        *resourceReservedOffset,
        limits.minResourceHeapReservedRange,
        *resourceHeapSize,
        *samplerStride,
        *samplerRegionSize,
        *samplerReservedOffset,
        limits.minSamplerHeapReservedRange,
        *samplerHeapSize};
}

} // namespace ar::rhi::detail
