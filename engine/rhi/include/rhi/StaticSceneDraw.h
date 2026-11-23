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
    // Three column-major vec4 columns (world-space camera rotation, padded to 4 floats each).
    std::array<float, 12> viewToWorld{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
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

} // namespace ar::rhi
