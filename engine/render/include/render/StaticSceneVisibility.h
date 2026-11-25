#pragma once

#include <array>
#include <span>
#include <vector>

#include "render/StaticSceneDrawData.h"
#include "rhi/StaticSceneDraw.h"

namespace ar::render {

// Builds reusable per-instance culling records from packed scene draw data so per-frame CPU
// frustum culling operates over a compact, contiguous array with zero allocations.
[[nodiscard]] std::vector<rhi::CpuCullInstance>
buildCpuCullInstances(const StaticSceneDrawData& scene);

// Tests local AABBs against the 6 Reverse-Z clip half-spaces of (scaleX(viewProj, inverseAspect) *
// modelMatrix), matching shaders/static_indirect.comp exactly. Writes up to outCommands.size()
// visible draw commands while still reporting the true unclamped visibleCount.
[[nodiscard]] rhi::CpuCullResult cullStaticSceneCpu(
    std::span<const rhi::CpuCullInstance> instances,
    const std::array<float, 16>& clipFromWorld,
    float inverseAspect,
    std::span<rhi::IndexedIndirectDrawCommand> outCommands) noexcept;

} // namespace ar::render
