#include "rhi/RhiVerification.h"

// A public header must compile on its own; this translation unit uses the declared types and
// verifies every free-function signature without pulling in Vulkan headers.
namespace {

ar::rhi::DescriptorHeapSampleQuery makeQuery() {
    return ar::rhi::DescriptorHeapSampleQuery{1u, 2u, 0.25F, 0.75F};
}

ar::rhi::DescriptorHeapProbeResult makeProbeResult() {
    ar::rhi::DescriptorHeapProbeResult result;
    result.resourceHeapAddress = 4096u;
    result.samplerHeapAddress = 8192u;
    return result;
}

ar::rhi::DescriptorHeapRetirementResult makeRetirementResult() {
    ar::rhi::DescriptorHeapRetirementResult result;
    result.retiredImageSlot = 1u;
    result.reusedImageSlot = 1u;
    return result;
}

using VerifyGpuCullFn = std::expected<std::array<std::uint32_t, 2>, ar::core::Error> (*)(
    ar::rhi::RhiSession&,
    std::span<const std::byte>,
    std::span<const std::byte>,
    std::span<const std::uint32_t>,
    std::span<std::uint32_t>,
    std::span<std::array<std::uint32_t, 4>>);

using VerifyDescriptorHeapFn =
    std::expected<ar::rhi::DescriptorHeapProbeResult, ar::core::Error> (*)(
        ar::rhi::RhiSession&,
        std::span<const std::uint32_t>,
        std::span<const ar::rhi::DescriptorHeapSampleQuery>);

using VerifyDescriptorHeapRetirementFn =
    std::expected<ar::rhi::DescriptorHeapRetirementResult, ar::core::Error> (*)(
        ar::rhi::RhiSession&, std::span<const std::uint32_t>);

constexpr VerifyGpuCullFn kVerifyGpuCull = &ar::rhi::verifyGpuCull;
constexpr VerifyDescriptorHeapFn kVerifyDescriptorHeap = &ar::rhi::verifyDescriptorHeap;
constexpr VerifyDescriptorHeapRetirementFn kVerifyDescriptorHeapRetirement =
    &ar::rhi::verifyDescriptorHeapRetirement;

} // namespace
