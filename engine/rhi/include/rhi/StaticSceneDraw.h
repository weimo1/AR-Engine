#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace ar::rhi {

struct StaticVertex {
    std::array<float, 3> position;
    std::array<float, 2> uv;
    std::array<float, 3> normal{};
};

// Views are consumed synchronously. Identity refers to immutable mesh and texture data.
struct StaticMeshUpload {
    std::shared_ptr<const void> identity;
    std::span<const StaticVertex> vertices;
    std::span<const std::uint32_t> indices;
    std::span<const std::uint8_t> rgba;
    std::uint32_t width;
    std::uint32_t height;
    std::span<const std::uint8_t> materialRgba{};
    std::uint32_t materialWidth = 0;
    std::uint32_t materialHeight = 0;
};

struct FrameView {
    // Column-major world -> view matrix (upper-left 3x3 is orthonormal camera rotation).
    std::array<float, 16> view{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Column-major projection(aspect=1) * view; Present supplies current aspect.
    std::array<float, 16> clipFromWorld{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Column-major inverse(projection(aspect=1)); Present scales row 0 by aspect.
    std::array<float, 16> inverseProjection{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Column-major view -> world matrix (inverse(view) = translate(camPos) *
    // mat4_cast(orientation)).
    std::array<float, 16> viewToWorld{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Active camera clip planes for Reverse-Z depth debug visualization.
    float nearPlane = 0.1F;
    float farPlane = 50.F;
};

struct DirectionalLightingState {
    // True preserves legacy view-space headlamp L_view = (0, 0, 1) for existing non-shadow scenes.
    bool legacyViewSpace = true;
    // Unit world-space propagation direction (from light toward scene), used when !legacyViewSpace.
    std::array<float, 3> directionWorld{0.F, 0.F, -1.F};
    std::array<float, 3> color{1.F, 1.F, 1.F};
    float intensity = 6.0F;
    bool shadowEnabled = false;
    // Column-major lightProj * lightView, valid when shadowEnabled is true.
    std::array<float, 16> lightViewProj{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float normalBias = 0.02F;
    float depthBias = 0.0005F;
    float pcfRadiusTexels = 1.0F;
};

struct StaticInstanceDraw {
    std::uint32_t meshIndex;
    // Column-major world-space model transform.
    std::array<float, 16> modelMatrix;
    // World-space normal matrix (3 column-major vec4s; w components carry material factors).
    std::array<float, 12> normalFromModel{};
    std::array<float, 4> baseColorFactor{1, 1, 1, 1};
};

struct StaticIndexedRange {
    std::uint32_t firstIndex;
    std::uint32_t indexCount;
    std::int32_t vertexOffset;
    std::array<float, 3> minimum;
    std::array<float, 3> maximum;
};

struct StaticIndirectGeometry {
    std::span<const StaticVertex> vertices;
    std::span<const std::uint32_t> indices;
    std::span<const StaticIndexedRange> ranges;
    std::span<const std::uint64_t> objectIds;
};

enum class StaticCullMode { GpuCompute, CpuFrustum };

struct CpuCullInstance {
    std::array<float, 16> modelMatrix;
    std::array<float, 3> boundsMin;
    std::array<float, 3> boundsMax;
    std::uint32_t firstIndex;
    std::uint32_t indexCount;
    std::int32_t vertexOffset;
    std::uint32_t instanceIndex;
};

// Binary-identical to VkDrawIndexedIndirectCommand (20 bytes) so CPU culling writes directly into
// the per-frame indirect command buffer consumed by vkCmdDrawIndexedIndirectCount.
struct IndexedIndirectDrawCommand {
    std::uint32_t indexCount;
    std::uint32_t instanceCount;
    std::uint32_t firstIndex;
    std::int32_t vertexOffset;
    std::uint32_t firstInstance;
};

struct CpuCullResult {
    std::uint32_t visibleCount = 0;
    std::uint32_t drawCount = 0;
};

using CpuFrustumCullFn = CpuCullResult (*)(
    std::span<const CpuCullInstance> instances,
    const std::array<float, 16>& clipFromWorld,
    float inverseAspect,
    std::span<IndexedIndirectDrawCommand> outCommands) noexcept;

} // namespace ar::rhi
