#include <catch2/catch_test_macros.hpp>

#include <array>

#include "render/CullReference.h"

TEST_CASE("fixed scene reference handles visibility and capacity", "[render-culling-reference]") {
    const auto scene = ar::render::fixedCullScene();
    std::array<std::uint32_t, 4> output{99, 99, 99, 99};
    const auto mixed = ar::render::cullReference(scene.frustum, scene.objects, output);
    CHECK(mixed.visible == 3);
    CHECK(mixed.written == 3);
    CHECK(output == std::array<std::uint32_t, 4>{1, 3, 4, 99});

    const auto capped =
        ar::render::cullReference(scene.frustum, scene.objects, std::span{output}.first(2));
    CHECK(capped.visible == 3);
    CHECK(capped.written == 2);
    const auto exact =
        ar::render::cullReference(scene.frustum, scene.objects, std::span{output}.first(3));
    CHECK(exact.visible == exact.written);

    const auto zero = ar::render::cullReference(scene.frustum, scene.objects, {});
    CHECK(zero.visible == 3);
    CHECK(zero.written == 0);
    const auto empty = ar::render::cullReference(scene.frustum, {}, output);
    CHECK(empty.visible == 0);
    CHECK(empty.written == 0);
    const auto outside =
        ar::render::cullReference(scene.frustum, std::span{scene.objects}.subspan(1, 1), output);
    CHECK(outside.visible == 0);
    const auto inside =
        ar::render::cullReference(scene.frustum, std::span{scene.objects}.first(1), output);
    CHECK(inside.visible == 1);
    CHECK(inside.written == 1);
}
