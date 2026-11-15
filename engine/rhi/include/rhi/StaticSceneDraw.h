#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace ar::rhi {

struct StaticVertex {
    std::array<float, 3> position;
    std::array<float, 2> uv;
};

// Views are consumed synchronously. Identity refers to immutable mesh and texture data.
struct StaticMeshUpload {
    std::shared_ptr<const void> identity;
    std::span<const StaticVertex> vertices;
    std::span<const std::uint32_t> indices;
    std::span<const std::uint8_t> rgba;
    std::uint32_t width;
    std::uint32_t height;
};

struct StaticInstanceDraw {
    std::uint32_t meshIndex;
    // Column-major with projection aspect 1. Present supplies current aspect.
    std::array<float, 16> clipFromModel;
};

} // namespace ar::rhi
