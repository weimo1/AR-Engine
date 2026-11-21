#include <catch2/catch_test_macros.hpp>

#include "assets/StaticAssetCache.h"
#include "render/StaticSceneDrawData.h"

namespace {

constexpr std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

std::shared_ptr<ar::assets::StaticMesh> triangle() {
    auto mesh = std::make_shared<ar::assets::StaticMesh>();
    mesh->vertices = {{{0, 0, 0}, {0, 0}}, {{1, 0, 0}, {1, 0}}, {{0, 1, 0}, {0, 1}}};
    mesh->indices = {0, 1, 2};
    return mesh;
}

} // namespace

TEST_CASE(
    "packed scene preserves indexed mesh ranges and instance identity",
    "[render-static-scene-draw-data]") {
    const auto shared = triangle();
    auto second = triangle();
    second->normals.assign(3, {0, 0, 1});
    second->indices = {1, 2, 0};
    second->material.metallicFactor = 0.25F;
    auto scaled = identity;
    scaled[0] = 2.F;
    scaled[12] = 4.F;
    const auto packed = ar::render::packStaticSceneDrawData(
        {{}, {{10, identity, shared}, {20, scaled, shared}, {30, identity, second}}});
    REQUIRE(packed);
    REQUIRE(packed->meshes.size() == 2);
    REQUIRE(packed->vertices.size() == 6);
    CHECK(packed->vertices[0].normal == std::array<float, 3>{0, 0, 1});
    CHECK(packed->indices == std::vector<std::uint32_t>{0, 1, 2, 1, 2, 0});
    CHECK(packed->meshes[1].firstIndex == 3);
    CHECK(packed->meshes[1].indexCount == 3);
    CHECK(packed->meshes[1].vertexOffset == 3);
    CHECK(packed->meshes[1].maximum == std::array<float, 3>{1, 1, 0});
    REQUIRE(packed->instances.size() == 3);
    CHECK(packed->instances[1].objectId == 20);
    CHECK(packed->instances[1].meshIndex == 0);
    CHECK(packed->instances[1].modelMatrix == scaled);
    CHECK(packed->instances[1].normalMatrix[0] == 0.5F);
    CHECK(packed->instances[2].materialIndex == 1);
    REQUIRE(packed->materials.size() == 2);
    CHECK(packed->materials[1].metallicFactor == 0.25F);
    CHECK(packed->materials[1].source == second);
}

TEST_CASE(
    "packed scene rejects invalid indices and smaller capacity",
    "[render-static-scene-draw-data]") {
    const auto empty = ar::render::packStaticSceneDrawData({});
    REQUIRE(empty);
    CHECK(empty->instances.empty());

    auto mesh = triangle();
    const ar::render::StaticRenderInput input{{}, {{10, identity, mesh}}};
    CHECK_FALSE(ar::render::packStaticSceneDrawData(input, {2, 3, 1}));
    CHECK_FALSE(ar::render::packStaticSceneDrawData(input, {3, 2, 1}));
    CHECK_FALSE(ar::render::packStaticSceneDrawData(input, {3, 3, 0}));
    mesh->indices.back() = 3;
    CHECK_FALSE(ar::render::packStaticSceneDrawData(input));
}
