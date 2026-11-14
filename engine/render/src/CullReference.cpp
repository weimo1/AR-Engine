#include "render/CullReference.h"

#include <algorithm>

namespace ar::render {

CullScene fixedCullScene() noexcept {
    // The fixed camera uses an orthographic clip volume in Vulkan depth convention.
    CullScene scene{
        .frustum =
            {{{1.F, 0.F, 0.F, 1.F},
              {-1.F, 0.F, 0.F, 1.F},
              {0.F, 1.F, 0.F, 1.F},
              {0.F, -1.F, 0.F, 1.F},
              {0.F, 0.F, 1.F, 0.F},
              {0.F, 0.F, -1.F, 1.F}}},
        .objects =
            {{{{-0.5F, 0.F, 0.5F}, 0.125F, 1, 0, 3},
              {{1.5F, 0.F, 0.5F}, 0.125F, 2, 3, 3},
              {{1.F, 0.F, 0.5F}, 0.125F, 3, 6, 3},
              {{1.125F, 0.5F, 0.5F}, 0.125F, 4, 9, 3}}},
        .vertices = {}};
    for (const auto& object : scene.objects) {
        const auto [x, y, z] = object.center;
        scene.vertices[object.firstVertex] = {x - 0.125F, y, z};
        scene.vertices[object.firstVertex + 1] = {x + 0.0625F, y - 0.0625F, z};
        scene.vertices[object.firstVertex + 2] = {x + 0.0625F, y + 0.0625F, z};
    }
    return scene;
}

CullResult cullReference(
    const Frustum& frustum,
    std::span<const CullObject> objects,
    std::span<std::uint32_t> output) noexcept {
    CullResult result{};
    for (const auto& object : objects) {
        const auto [x, y, z] = object.center;
        if (std::ranges::any_of(frustum, [&](const auto& plane) {
                return plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < -object.radius;
            })) {
            continue;
        }
        ++result.visible;
        if (result.written < output.size()) {
            output[result.written++] = object.id;
        }
    }
    return result;
}

} // namespace ar::render
