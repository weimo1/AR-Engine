#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include <volk.h>

#include "binding/DescriptorHeapTable.h"
#include "core/Error.h"
#include "rhi/RhiSession.h"

namespace ar::rhi::detail {

class DeviceResources;

[[nodiscard]] std::expected<DescriptorHeapProbeResult, core::Error> runDescriptorHeapProbe(
    VkPhysicalDevice physical,
    VkDevice device,
    VkQueue queue,
    std::uint32_t queueFamily,
    DeviceResources& resources,
    const DescriptorHeapLimits& limits,
    const DescriptorHeapDispatch& dispatch,
    std::span<const std::uint32_t> computeShader,
    std::span<const DescriptorHeapSampleQuery> queries);

} // namespace ar::rhi::detail
