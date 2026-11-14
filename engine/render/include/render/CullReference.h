#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ar::render {

using Position = std::array<float, 3>;
using Frustum = std::array<std::array<float, 4>, 6>;

struct alignas(16) CullObject {
    Position center;
    float radius;
    std::uint32_t id;
    std::uint32_t firstVertex;
    std::uint32_t vertexCount;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(Frustum::value_type) == 16 && sizeof(Frustum) == 96);
static_assert(sizeof(CullObject) == 32 && alignof(CullObject) == 16);
static_assert(offsetof(CullObject, center) == 0);
static_assert(offsetof(CullObject, radius) == 12);
static_assert(offsetof(CullObject, id) == 16);
static_assert(offsetof(CullObject, firstVertex) == 20);
static_assert(offsetof(CullObject, vertexCount) == 24);
static_assert(offsetof(CullObject, reserved) == 28);

struct CullScene {
    Frustum frustum;
    std::array<CullObject, 4> objects;
    std::array<Position, 12> vertices;
};

struct CullResult {
    std::size_t visible;
    std::size_t written;
};

CullScene fixedCullScene() noexcept;
CullResult cullReference(
    const Frustum&, std::span<const CullObject>, std::span<std::uint32_t> output) noexcept;

} // namespace ar::render
