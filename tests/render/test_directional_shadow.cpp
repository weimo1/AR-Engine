#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <utility>

#include "assets/StaticAssetCache.h"
#include "render/DirectionalShadow.h"
#include "render/PresentationGraph.h"
#include "render/StaticSceneRenderer.h"
#include "scene/StaticScene.h"

namespace {

namespace fs = std::filesystem;

std::array<float, 4>
mulMat4Vec4(const std::array<float, 16>& matrix, const std::array<float, 4>& vector) {
    std::array<float, 4> out{};
    for (std::size_t row = 0; row < 4; ++row) {
        out[row] = matrix[row] * vector[0] + matrix[4 + row] * vector[1] +
                   matrix[8 + row] * vector[2] + matrix[12 + row] * vector[3];
    }
    return out;
}

std::array<float, 16>
mulMat4Mat4(const std::array<float, 16>& lhs, const std::array<float, 16>& rhs) {
    std::array<float, 16> out{};
    for (std::size_t col = 0; col < 4; ++col) {
        const auto column =
            mulMat4Vec4(lhs, {rhs[col * 4], rhs[col * 4 + 1], rhs[col * 4 + 2], rhs[col * 4 + 3]});
        for (std::size_t row = 0; row < 4; ++row) {
            out[col * 4 + row] = column[row];
        }
    }
    return out;
}

fs::path testOutputRoot() {
#ifdef _WIN32
    wchar_t output[4096]{};
    std::size_t length = 0;
    REQUIRE(_wgetenv_s(&length, output, std::size(output), L"AR_TEST_OUTPUT_ROOT") == 0);
    REQUIRE(length > 1);
    return output;
#else
    const auto* output = std::getenv("AR_TEST_OUTPUT_ROOT");
    REQUIRE(output != nullptr);
    return output;
#endif
}

struct TempDir {
    fs::path path;

    explicit TempDir(const fs::path& root)
        : path(
              root /
              ("shadow-scene-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

} // namespace

TEST_CASE(
    "DirectionalShadow fits scene bounds across light directions and enforces contracts",
    "[render-directional-shadow]") {
    SECTION(
        "multi-direction light cameras map all 8 corners into NDC [-1,1]^2 x [0,1] with near < "
        "far") {
        const ar::render::SceneWorldBounds bounds{{-5.0F, -0.2F, -4.7F}, {5.0F, 4.0F, 4.5F}};
        const std::array<std::array<float, 3>, 5> directions{{
            {-0.4F, -0.85F, -0.35F},
            {0.6F, -0.7F, 0.4F},
            {-0.8F, -0.2F, 0.5F},
            {0.0F, -1.0F, 0.0F},
            {0.0F, 1.0F, 0.0F},
        }};

        for (const auto& dir : directions) {
            ar::render::DirectionalLight light{};
            light.direction = dir;
            const auto shadow = ar::render::buildDirectionalShadowView(bounds, light);
            REQUIRE(shadow.has_value());
            CHECK(shadow->zNear >= 0.1F);
            CHECK(shadow->zFar > shadow->zNear);

            const double len = std::sqrt(
                static_cast<double>(dir[0]) * dir[0] + static_cast<double>(dir[1]) * dir[1] +
                static_cast<double>(dir[2]) * dir[2]);
            const std::array<float, 3> forward{
                static_cast<float>(dir[0] / len),
                static_cast<float>(dir[1] / len),
                static_cast<float>(dir[2] / len)};

            float minDot = std::numeric_limits<float>::infinity();
            float maxDot = -std::numeric_limits<float>::infinity();
            float nearCornerDepth = 0.F;
            float farCornerDepth = 0.F;

            for (int mask = 0; mask < 8; ++mask) {
                const std::array<float, 4> corner{
                    (mask & 1) ? bounds.max[0] : bounds.min[0],
                    (mask & 2) ? bounds.max[1] : bounds.min[1],
                    (mask & 4) ? bounds.max[2] : bounds.min[2],
                    1.F};
                const auto clip = mulMat4Vec4(shadow->lightViewProj, corner);
                REQUIRE(std::abs(clip[3] - 1.F) < 1e-5F);
                const float ndcX = clip[0] / clip[3];
                const float ndcY = clip[1] / clip[3];
                const float ndcZ = clip[2] / clip[3];
                CHECK(ndcX >= -1.0F);
                CHECK(ndcX <= 1.0F);
                CHECK(ndcY >= -1.0F);
                CHECK(ndcY <= 1.0F);
                CHECK(ndcZ >= 0.0F);
                CHECK(ndcZ <= 1.0F);

                const float alongForward =
                    corner[0] * forward[0] + corner[1] * forward[1] + corner[2] * forward[2];
                if (alongForward < minDot) {
                    minDot = alongForward;
                    nearCornerDepth = ndcZ;
                }
                if (alongForward > maxDot) {
                    maxDot = alongForward;
                    farCornerDepth = ndcZ;
                }
            }
            CHECK(nearCornerDepth < farCornerDepth);
        }
    }

    SECTION("shadow matrices are invariant to main camera pose changes") {
        const ar::render::SceneWorldBounds bounds{{-4.F, 0.F, -4.F}, {4.F, 3.F, 4.F}};
        const auto light = ar::render::defaultDirectionalLight();
        const auto viewA = ar::render::buildDirectionalShadowView(bounds, light);
        REQUIRE(viewA.has_value());

        // Varying main camera pose changes FrameView but leaves buildDirectionalShadowView
        // identical.
        ar::render::StaticCamera cam1{};
        cam1.position = {0.F, 2.F, 5.F};
        ar::render::StaticCamera cam2 = cam1;
        cam2.position = {3.5F, 1.2F, -1.0F};
        cam2.orientation = {0.F, 0.3826834F, 0.F, 0.9238795F};

        const auto fv1 = ar::render::buildFrameView(cam1, true);
        const auto fv2 = ar::render::buildFrameView(cam2, true);
        REQUIRE(fv1.has_value());
        REQUIRE(fv2.has_value());
        CHECK(fv1->view != fv2->view);

        // Full 4x4 view * viewToWorld == I for rotated/translated camera.
        const auto product = mulMat4Mat4(fv2->view, fv2->viewToWorld);
        for (std::size_t col = 0; col < 4; ++col) {
            for (std::size_t row = 0; row < 4; ++row) {
                const float expected = col == row ? 1.F : 0.F;
                CHECK(product[col * 4 + row] == Catch::Approx(expected).margin(1e-5F));
            }
        }

        const auto viewB = ar::render::buildDirectionalShadowView(bounds, light);
        REQUIRE(viewB.has_value());
        CHECK(viewA->lightViewProj == viewB->lightViewProj);
        CHECK(viewA->lightView == viewB->lightView);
        CHECK(viewA->lightProj == viewB->lightProj);
    }

    SECTION("computeSceneWorldBounds handles empty scenes, transformed meshes, and invalid draws") {
        ar::render::StaticRenderInput emptyInput{};
        const auto emptyBounds = ar::render::computeSceneWorldBounds(emptyInput);
        REQUIRE(emptyBounds.has_value());
        CHECK(emptyBounds->min == std::array{-1.F, -1.F, -1.F});
        CHECK(emptyBounds->max == std::array{1.F, 1.F, 1.F});

        auto mesh = std::make_shared<ar::assets::StaticMesh>();
        mesh->vertices = {
            {{-0.5F, -0.5F, -0.5F}, {0.F, 0.F}},
            {{0.5F, 0.5F, 0.5F}, {1.F, 1.F}},
        };
        ar::render::StaticRenderInput sceneInput{};
        // Scale by (2, 4, 2) and translate by (3, 2, -1) -> world AABB [2,0,-2]..[4,4,0].
        sceneInput.draws.push_back(
            {1u,
             {2.F, 0.F, 0.F, 0.F, 0.F, 4.F, 0.F, 0.F, 0.F, 0.F, 2.F, 0.F, 3.F, 2.F, -1.F, 1.F},
             mesh});
        const auto computed = ar::render::computeSceneWorldBounds(sceneInput);
        REQUIRE(computed.has_value());
        CHECK(computed->min[0] == Catch::Approx(2.F));
        CHECK(computed->min[1] == Catch::Approx(0.F));
        CHECK(computed->min[2] == Catch::Approx(-2.F));
        CHECK(computed->max[0] == Catch::Approx(4.F));
        CHECK(computed->max[1] == Catch::Approx(4.F));
        CHECK(computed->max[2] == Catch::Approx(0.F));

        ar::render::StaticRenderInput nullMeshInput{};
        nullMeshInput.draws.push_back(
            {1u, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, nullptr});
        CHECK_FALSE(ar::render::computeSceneWorldBounds(nullMeshInput).has_value());
    }

    SECTION("four-quadrant directional light precedence and invalid parameter rejection") {
        // Quadrant 1: no scene light, shadow pipeline off -> legacy view-space headlamp.
        const auto q1 = ar::render::resolveDirectionalLight(std::nullopt, false);
        REQUIRE(q1.has_value());
        CHECK(q1->legacyViewSpace);
        CHECK_FALSE(q1->castShadows);
        CHECK(q1->intensity == Catch::Approx(6.0F));

        // Quadrant 2: no scene light, shadow pipeline on -> default world directional light.
        const auto q2 = ar::render::resolveDirectionalLight(std::nullopt, true);
        REQUIRE(q2.has_value());
        CHECK_FALSE(q2->legacyViewSpace);
        CHECK(q2->castShadows);
        CHECK(q2->directionWorld == ar::render::defaultDirectionalLight().direction);

        // Quadrant 3: scene light present, shadow pipeline off -> world light, castShadows forced
        // false.
        ar::render::DirectionalLight custom{{0.F, -2.F, 0.F}, {1.F, 0.9F, 0.8F}, 4.5F, true};
        const auto q3 = ar::render::resolveDirectionalLight(custom, false);
        REQUIRE(q3.has_value());
        CHECK_FALSE(q3->legacyViewSpace);
        CHECK_FALSE(q3->castShadows);
        CHECK(q3->directionWorld[1] == Catch::Approx(-1.F));
        CHECK(q3->color == custom.color);
        CHECK(q3->intensity == Catch::Approx(4.5F));

        // Quadrant 4a/4b: scene light present, shadow pipeline on -> respects light.castShadows.
        const auto q4On = ar::render::resolveDirectionalLight(custom, true);
        REQUIRE(q4On.has_value());
        CHECK_FALSE(q4On->legacyViewSpace);
        CHECK(q4On->castShadows);

        custom.castShadows = false;
        const auto q4Off = ar::render::resolveDirectionalLight(custom, true);
        REQUIRE(q4Off.has_value());
        CHECK_FALSE(q4Off->legacyViewSpace);
        CHECK_FALSE(q4Off->castShadows);

        // Invalid directional light / config / bounds rejection.
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const ar::render::SceneWorldBounds validBounds{{-1.F, -1.F, -1.F}, {1.F, 1.F, 1.F}};
        const auto validLight = ar::render::defaultDirectionalLight();

        auto zeroDir = validLight;
        zeroDir.direction = {0.F, 0.F, 0.F};
        CHECK_FALSE(ar::render::buildDirectionalShadowView(validBounds, zeroDir).has_value());
        CHECK_FALSE(ar::render::resolveDirectionalLight(zeroDir, true).has_value());

        auto negIntensity = validLight;
        negIntensity.intensity = -0.1F;
        CHECK_FALSE(ar::render::buildDirectionalShadowView(validBounds, negIntensity).has_value());

        auto negColor = validLight;
        negColor.color = {1.F, -0.01F, 1.F};
        CHECK_FALSE(ar::render::buildDirectionalShadowView(validBounds, negColor).has_value());

        auto nanDir = validLight;
        nanDir.direction[0] = nan;
        CHECK_FALSE(ar::render::buildDirectionalShadowView(validBounds, nanDir).has_value());

        ar::render::DirectionalShadowConfig smallRes{};
        smallRes.resolution = 32u;
        CHECK_FALSE(
            ar::render::buildDirectionalShadowView(validBounds, validLight, smallRes).has_value());

        ar::render::DirectionalShadowConfig hugeRes{};
        hugeRes.resolution = 8192u;
        CHECK_FALSE(
            ar::render::buildDirectionalShadowView(validBounds, validLight, hugeRes).has_value());

        ar::render::DirectionalShadowConfig negBias{};
        negBias.normalBias = -0.01F;
        CHECK_FALSE(
            ar::render::buildDirectionalShadowView(validBounds, validLight, negBias).has_value());

        const ar::render::SceneWorldBounds invertedBounds{{2.F, 0.F, 0.F}, {1.F, 1.F, 1.F}};
        CHECK_FALSE(ar::render::buildDirectionalShadowView(invertedBounds, validLight).has_value());
    }

    SECTION("StaticScene optional directional_light round-trips and keeps v1 scenes compatible") {
        const TempDir temp(testOutputRoot());

        const fs::path legacyPath = temp.path / "legacy.json";
        {
            std::ofstream out(legacyPath, std::ios::binary);
            out << R"({"version":1,"camera":{"position":[0,1,4],"orientation":[0,0,0,1],"fov_y":0.7853982,"near":0.1,"far":50.0},"objects":[]})";
            REQUIRE(out.good());
        }
        const auto loadedLegacy = ar::scene::StaticScene::load(legacyPath);
        REQUIRE(loadedLegacy.has_value());
        CHECK_FALSE(loadedLegacy->directionalLight().has_value());

        const fs::path savedLegacy = temp.path / "legacy_saved.json";
        REQUIRE(loadedLegacy->save(savedLegacy).has_value());
        const auto reloadedLegacy = ar::scene::StaticScene::load(savedLegacy);
        REQUIRE(reloadedLegacy.has_value());
        CHECK_FALSE(reloadedLegacy->directionalLight().has_value());

        const fs::path litPath = temp.path / "lit.json";
        {
            std::ofstream out(litPath, std::ios::binary);
            out << R"({"version":1,"camera":{"position":[0,1,4],"orientation":[0,0,0,1],"fov_y":0.7853982,"near":0.1,"far":50.0},"directional_light":{"direction":[-0.3,-0.9,-0.2],"color":[1.0,0.95,0.9],"intensity":5.5,"cast_shadows":false},"objects":[]})";
            REQUIRE(out.good());
        }
        const auto loadedLit = ar::scene::StaticScene::load(litPath);
        REQUIRE(loadedLit.has_value());
        REQUIRE(loadedLit->directionalLight().has_value());
        CHECK(loadedLit->directionalLight()->direction == std::array{-0.3F, -0.9F, -0.2F});
        CHECK(loadedLit->directionalLight()->color == std::array{1.0F, 0.95F, 0.9F});
        CHECK(loadedLit->directionalLight()->intensity == Catch::Approx(5.5F));
        CHECK_FALSE(loadedLit->directionalLight()->castShadows);

        const fs::path savedLit = temp.path / "lit_saved.json";
        REQUIRE(loadedLit->save(savedLit).has_value());
        const auto reloadedLit = ar::scene::StaticScene::load(savedLit);
        REQUIRE(reloadedLit.has_value());
        CHECK(reloadedLit->directionalLight() == loadedLit->directionalLight());

        // Reject invalid directional_light payloads.
        const std::array<const char*, 4> invalidLights{
            R"({"direction":[0,0,0]})",
            R"({"direction":[0,-1,0],"color":[-1,1,1]})",
            R"({"direction":[0,-1,0],"intensity":-1.0})",
            R"({"direction":[0,-1,0],"cast_shadows":"yes"})",
        };
        for (std::size_t i = 0; i < invalidLights.size(); ++i) {
            const fs::path badPath = temp.path / ("bad_light_" + std::to_string(i) + ".json");
            {
                std::ofstream out(badPath, std::ios::binary);
                out << R"({"version":1,"camera":{"position":[0,1,4],"orientation":[0,0,0,1],"fov_y":0.7853982,"near":0.1,"far":50.0},"directional_light":)"
                    << invalidLights[i] << R"(,"objects":[]})";
                REQUIRE(out.good());
            }
            const auto bad = ar::scene::StaticScene::load(badPath);
            REQUIRE_FALSE(bad.has_value());
            CHECK(bad.error().stage == ar::scene::FileStage::Validate);
        }
    }

    SECTION("PresentationGraph plans ShadowDepth Sync2 barriers only when shadows are enabled") {
        using namespace ar::rhi;
        PresentationGraphInput off{};
        off.lighting = true;
        off.shadows = false;
        const auto planOff = ar::render::planPresentationGraph(off);
        REQUIRE(planOff.has_value());
        CHECK(planOff->barriers[std::to_underlying(PresentationPass::Shadow)].empty());
        for (const auto& passBarriers : planOff->barriers) {
            for (const auto& barrier : passBarriers) {
                CHECK(barrier.image != PresentationImage::ShadowDepth);
            }
        }

        PresentationGraphInput on{};
        on.lighting = true;
        on.shadows = true;
        const auto planOn = ar::render::planPresentationGraph(on);
        REQUIRE(planOn.has_value());

        const auto& shadowBarriers = planOn->barriers[std::to_underlying(PresentationPass::Shadow)];
        REQUIRE(shadowBarriers.size() == 1u);
        CHECK(shadowBarriers[0].image == PresentationImage::ShadowDepth);
        CHECK(shadowBarriers[0].after == GraphLayout::DepthAttachment);
        CHECK(shadowBarriers[0].discard);
        CHECK((shadowBarriers[0].destination & std::to_underlying(GraphUsage::DepthWrite)) != 0u);

        const auto& lightingBarriers =
            planOn->barriers[std::to_underlying(PresentationPass::Lighting)];
        bool foundShadowRead = false;
        for (const auto& barrier : lightingBarriers) {
            if (barrier.image == PresentationImage::ShadowDepth) {
                foundShadowRead = true;
                CHECK(barrier.before == GraphLayout::DepthAttachment);
                CHECK(barrier.after == GraphLayout::ShaderRead);
                CHECK_FALSE(barrier.discard);
                CHECK((barrier.source & std::to_underlying(GraphUsage::DepthWrite)) != 0u);
                CHECK((barrier.destination & std::to_underlying(GraphUsage::FragmentSample)) != 0u);
            }
        }
        CHECK(foundShadowRead);
        CHECK(
            planOn->finalStates[std::to_underlying(PresentationImage::ShadowDepth)].layout ==
            GraphLayout::ShaderRead);

        // Shadows without lighting is rejected.
        PresentationGraphInput invalid{};
        invalid.lighting = false;
        invalid.shadows = true;
        const auto rejected = ar::render::planPresentationGraph(invalid);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().category == ar::core::ErrorCategory::InvalidArguments);
    }
}
