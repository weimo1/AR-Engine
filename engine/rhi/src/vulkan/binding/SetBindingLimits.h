#pragma once

#include <cstdint>

namespace ar::rhi::detail {

struct SetBindingLimits {
    bool nonUniformIndexing;
    std::uint32_t images;
    std::uint32_t samplers;
    std::uint32_t total;
};

enum class SetBindingCheck { Ready, MissingFeature, ImageLimit, SamplerLimit, StageLimit };

inline constexpr std::uint32_t kSetImageCount = 8;
inline constexpr std::uint32_t kSetSamplerCount = 4;

constexpr SetBindingCheck
checkSetBinding(SetBindingLimits limits, std::uint32_t images, std::uint32_t samplers) noexcept {
    if (!limits.nonUniformIndexing) {
        return SetBindingCheck::MissingFeature;
    }
    if (images == 0 || images > limits.images) {
        return SetBindingCheck::ImageLimit;
    }
    if (samplers == 0 || samplers > limits.samplers) {
        return SetBindingCheck::SamplerLimit;
    }
    if (static_cast<std::uint64_t>(images) + samplers > limits.total) {
        return SetBindingCheck::StageLimit;
    }
    return SetBindingCheck::Ready;
}

} // namespace ar::rhi::detail
