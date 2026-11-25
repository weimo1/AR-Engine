#include "render/StaticSceneVisibility.h"

#include <cmath>
#include <cstddef>

namespace ar::render {
namespace {

bool isInstanceVisible(
    const std::array<float, 16>& vp,
    const std::array<float, 16>& m,
    const std::array<float, 3>& bmin,
    const std::array<float, 3>& bmax) noexcept {
    // Compute rows of column-major (vp * m):
    // row r, col c = sum_{k=0..3} vp[k * 4 + r] * m[c * 4 + k].
    const auto rowDot = [&](std::size_t r, std::size_t c) noexcept -> float {
        const std::size_t base = c * 4u;
        return vp[r] * m[base] + vp[4u + r] * m[base + 1u] + vp[8u + r] * m[base + 2u] +
               vp[12u + r] * m[base + 3u];
    };

    const std::array<float, 4> x{rowDot(0u, 0u), rowDot(0u, 1u), rowDot(0u, 2u), rowDot(0u, 3u)};
    const std::array<float, 4> y{rowDot(1u, 0u), rowDot(1u, 1u), rowDot(1u, 2u), rowDot(1u, 3u)};
    const std::array<float, 4> z{rowDot(2u, 0u), rowDot(2u, 1u), rowDot(2u, 2u), rowDot(2u, 3u)};
    const std::array<float, 4> w{rowDot(3u, 0u), rowDot(3u, 1u), rowDot(3u, 2u), rowDot(3u, 3u)};

    const auto insideHalfSpace = [&](float px, float py, float pz, float pw) noexcept -> bool {
        const float fx = px >= 0.F ? bmax[0] : bmin[0];
        const float fy = py >= 0.F ? bmax[1] : bmin[1];
        const float fz = pz >= 0.F ? bmax[2] : bmin[2];
        return (px * fx + py * fy + pz * fz + pw) >= 0.F;
    };

    // Match shaders/static_indirect.comp plane order: w+x, w-x, w+y, w-y, z, w-z (Reverse-Z NDC
    // satisfies 0 <= z <= w).
    if (!insideHalfSpace(w[0] + x[0], w[1] + x[1], w[2] + x[2], w[3] + x[3])) {
        return false;
    }
    if (!insideHalfSpace(w[0] - x[0], w[1] - x[1], w[2] - x[2], w[3] - x[3])) {
        return false;
    }
    if (!insideHalfSpace(w[0] + y[0], w[1] + y[1], w[2] + y[2], w[3] + y[3])) {
        return false;
    }
    if (!insideHalfSpace(w[0] - y[0], w[1] - y[1], w[2] - y[2], w[3] - y[3])) {
        return false;
    }
    if (!insideHalfSpace(z[0], z[1], z[2], z[3])) {
        return false;
    }
    if (!insideHalfSpace(w[0] - z[0], w[1] - z[1], w[2] - z[2], w[3] - z[3])) {
        return false;
    }
    return true;
}

} // namespace

std::vector<rhi::CpuCullInstance> buildCpuCullInstances(const StaticSceneDrawData& scene) {
    std::vector<rhi::CpuCullInstance> out;
    out.reserve(scene.instances.size());
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        const auto& instance = scene.instances[i];
        if (instance.meshIndex >= scene.meshes.size()) {
            continue;
        }
        const auto& mesh = scene.meshes[instance.meshIndex];
        out.push_back(
            rhi::CpuCullInstance{
                instance.modelMatrix,
                mesh.minimum,
                mesh.maximum,
                mesh.firstIndex,
                mesh.indexCount,
                mesh.vertexOffset,
                static_cast<std::uint32_t>(i)});
    }
    return out;
}

rhi::CpuCullResult cullStaticSceneCpu(
    std::span<const rhi::CpuCullInstance> instances,
    const std::array<float, 16>& clipFromWorld,
    float inverseAspect,
    std::span<rhi::IndexedIndirectDrawCommand> outCommands) noexcept {
    rhi::CpuCullResult result{};
    if (instances.empty() || !std::isfinite(inverseAspect) || inverseAspect <= 0.F) {
        return result;
    }
    std::array<float, 16> scaledViewProj = clipFromWorld;
    for (std::size_t col = 0u; col < 4u; ++col) {
        scaledViewProj[col * 4u] *= inverseAspect;
    }

    const std::size_t capacity = outCommands.size();
    for (const auto& instance : instances) {
        if (!isInstanceVisible(
                scaledViewProj, instance.modelMatrix, instance.boundsMin, instance.boundsMax)) {
            continue;
        }
        const std::uint32_t slot = result.visibleCount++;
        if (slot < capacity) {
            outCommands[slot] = rhi::IndexedIndirectDrawCommand{
                instance.indexCount,
                1u,
                instance.firstIndex,
                instance.vertexOffset,
                instance.instanceIndex};
            ++result.drawCount;
        }
    }
    return result;
}

} // namespace ar::render
