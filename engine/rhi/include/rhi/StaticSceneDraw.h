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

struct StaticInstanceDraw {
    std::uint32_t meshIndex;
    // Column-major with projection aspect 1. Present supplies current aspect.
    std::array<float, 16> clipFromModel;
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
