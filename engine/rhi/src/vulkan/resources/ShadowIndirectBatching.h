#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ar::rhi::detail {

struct ShadowIndirectBatch {
    std::uint32_t firstCommand = 0;
    std::uint32_t commandCount = 0;

    bool operator==(const ShadowIndirectBatch&) const = default;
};

template <class Fn>
inline void forEachShadowIndirectBatch(
    std::uint32_t instanceCount, std::uint32_t maxDrawIndirectCount, Fn&& fn) {
    if (instanceCount == 0u || maxDrawIndirectCount == 0u) {
        return;
    }
    std::uint32_t offset = 0u;
    while (offset < instanceCount) {
        const std::uint32_t count = (std::min)(maxDrawIndirectCount, instanceCount - offset);
        fn(ShadowIndirectBatch{offset, count});
        offset += count;
    }
}

[[nodiscard]] inline std::vector<ShadowIndirectBatch>
planShadowIndirectBatches(std::uint32_t instanceCount, std::uint32_t maxDrawIndirectCount) {
    std::vector<ShadowIndirectBatch> batches;
    if (instanceCount == 0u || maxDrawIndirectCount == 0u) {
        return batches;
    }
    const std::uint32_t batchCount = (instanceCount - 1u) / maxDrawIndirectCount + 1u;
    batches.reserve(batchCount);
    forEachShadowIndirectBatch(instanceCount, maxDrawIndirectCount, [&](ShadowIndirectBatch batch) {
        batches.push_back(batch);
    });
    return batches;
}

} // namespace ar::rhi::detail
