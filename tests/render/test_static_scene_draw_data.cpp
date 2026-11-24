#include <catch2/catch_test_macros.hpp>

#include <limits>

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

TEST_CASE(
    "packStaticSceneDrawData validates mesh topology upfront and packs many meshes cleanly",
    "[render-static-scene-draw-data]") {
    const auto expectRejected = [](const std::shared_ptr<ar::assets::StaticMesh>& badMesh) {
        const ar::render::StaticRenderInput input{{}, {{10, identity, badMesh}}};
        const auto packed = ar::render::packStaticSceneDrawData(input);
        REQUIRE_FALSE(packed.has_value());
        CHECK(packed.error().category == ar::core::ErrorCategory::InvalidArguments);
        CHECK(packed.error().stage == ar::core::ErrorStage::Validation);
    };

    SECTION("incomplete trailing triangle is rejected before vertex access") {
        auto mesh = triangle();
        mesh->indices = {0, 1, 2, 0};
        expectRejected(mesh);
    }

    SECTION("out-of-range vertex index is rejected before vertex access") {
        auto mesh = triangle();
        mesh->indices = {0, 1, 3};
        expectRejected(mesh);
    }

    SECTION("short normal array is rejected before normal access") {
        auto mesh = triangle();
        mesh->normals = {{0, 0, 1}, {0, 0, 1}};
        expectRejected(mesh);
    }

    SECTION("non-finite vertex, material factor, or degenerate-only mesh is rejected") {
        auto nonFinite = triangle();
        nonFinite->vertices[1].position[0] = std::numeric_limits<float>::quiet_NaN();
        expectRejected(nonFinite);

        auto badMaterial = triangle();
        badMaterial->material.roughnessFactor = std::numeric_limits<float>::infinity();
        expectRejected(badMaterial);

        auto degenerate = triangle();
        degenerate->vertices = {{{0, 0, 0}, {0, 0}}, {{1, 0, 0}, {1, 0}}, {{2, 0, 0}, {1, 1}}};
        expectRejected(degenerate);
    }

    SECTION("many distinct small meshes pack consecutively with valid ranges") {
        ar::render::StaticRenderInput input{};
        constexpr std::size_t meshCount = 64;
        input.draws.reserve(meshCount);
        for (std::size_t i = 0; i < meshCount; ++i) {
            auto mesh = triangle();
            if (i % 2 == 1) {
                mesh->normals.assign(3, {0, 0, 1});
            }
            input.draws.push_back({i + 1, identity, std::move(mesh)});
        }
        const auto packed = ar::render::packStaticSceneDrawData(input);
        REQUIRE(packed.has_value());
        REQUIRE(packed->meshes.size() == meshCount);
        REQUIRE(packed->vertices.size() == meshCount * 3);
        REQUIRE(packed->indices.size() == meshCount * 3);
        for (std::size_t i = 0; i < meshCount; ++i) {
            CHECK(packed->meshes[i].firstIndex == i * 3);
            CHECK(packed->meshes[i].indexCount == 3);
            CHECK(packed->meshes[i].vertexOffset == static_cast<std::int32_t>(i * 3));
            CHECK(packed->vertices[i * 3].normal == std::array<float, 3>{0, 0, 1});
        }
    }
}
