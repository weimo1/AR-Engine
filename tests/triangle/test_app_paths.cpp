#include "AppPaths.h"

#include "assets/StaticAssetCache.h"
#include "render/StaticSceneDrawData.h"
#include "runtime/StaticSceneSession.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

namespace fs = std::filesystem;

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
              ("app-paths-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::optional<ar::app::LaunchOptions>
parseArgs(std::initializer_list<std::wstring_view> args, const ar::app::AppPaths& paths) {
    const std::vector<std::wstring_view> list(args);
    return ar::app::parseLaunchOptions(std::span<const std::wstring_view>(list), paths);
}

} // namespace

TEST_CASE(
    "AppPaths resolves exe-relative resources and showroom CLI rules",
    "[app-paths-and-showroom-cli]") {
    SECTION("resolveAppPaths anchors shaders and content to executable directory") {
        const fs::path exeDir = fs::path(u8"C:/nonexistent-ar-app/bin");
        const fs::path defaultOut = fs::path(u8"D:/default-output");
        const fs::path customOut = fs::path(u8"E:/custom-output");

        const auto resolvedDefault = ar::app::resolveAppPaths(exeDir, std::nullopt, defaultOut);
        REQUIRE(resolvedDefault.has_value());
        CHECK(resolvedDefault->shaders == exeDir / "shaders");
        CHECK(resolvedDefault->content == exeDir / "content");
        CHECK(resolvedDefault->output == defaultOut);

        const auto resolvedOverride = ar::app::resolveAppPaths(exeDir, customOut, defaultOut);
        REQUIRE(resolvedOverride.has_value());
        CHECK(resolvedOverride->shaders == exeDir / "shaders");
        CHECK(resolvedOverride->content == exeDir / "content");
        CHECK(resolvedOverride->output == customOut);

        CHECK_FALSE(ar::app::resolveAppPaths({}, std::nullopt, defaultOut).has_value());
        CHECK_FALSE(ar::app::resolveAppPaths(exeDir, std::nullopt, {}).has_value());
        CHECK_FALSE(ar::app::resolveAppPaths(exeDir, fs::path{}, defaultOut).has_value());

        const auto currentDir = ar::app::currentExecutableDirectory();
        REQUIRE(currentDir.has_value());
        CHECK(fs::is_directory(*currentDir));
    }

    SECTION("requireReadableFile and requireReadableDirectory report role and expanded path") {
        const TempDir temp(testOutputRoot());
        const fs::path sampleFile = temp.path / "probe.txt";
        {
            std::ofstream out(sampleFile, std::ios::binary);
            out << "ok";
            REQUIRE(out.good());
        }

        const auto okFile = ar::app::requireReadableFile(sampleFile, "showroom scene");
        REQUIRE(okFile.has_value());
        const auto okDir = ar::app::requireReadableDirectory(temp.path, "shader directory");
        REQUIRE(okDir.has_value());

        const fs::path missingFile = temp.path / "missing_scene.json";
        const auto errFile = ar::app::requireReadableFile(missingFile, "showroom scene");
        REQUIRE_FALSE(errFile.has_value());
        CHECK(errFile.error().find("showroom scene") != std::string::npos);
        CHECK(errFile.error().find("missing_scene.json") != std::string::npos);

        const fs::path missingDir = temp.path / "missing_shaders";
        const auto errDir = ar::app::requireReadableDirectory(missingDir, "shader directory");
        REQUIRE_FALSE(errDir.has_value());
        CHECK(errDir.error().find("shader directory") != std::string::npos);
        CHECK(errDir.error().find("missing_shaders") != std::string::npos);

        CHECK_FALSE(ar::app::requireReadableFile(temp.path, "scene file").has_value());
        CHECK_FALSE(ar::app::requireReadableDirectory(sampleFile, "asset root").has_value());
        CHECK_FALSE(ar::app::requireReadableFile({}, "scene file").has_value());
        CHECK_FALSE(ar::app::requireReadableDirectory({}, "asset root").has_value());
    }

    SECTION("parseLaunchOptions expands showroom defaults and enforces conflict matrix") {
        const ar::app::AppPaths paths{
            fs::path(u8"C:/ar-app/shaders"),
            fs::path(u8"C:/ar-app/content"),
            fs::path(u8"C:/ar-app/output"),
        };

        const auto showroom = parseArgs({L"--showroom"}, paths);
        REQUIRE(showroom.has_value());
        CHECK(showroom->showroom);
        CHECK(showroom->visible);
        CHECK_FALSE(showroom->frames.has_value());
        CHECK(showroom->scene == paths.content / "showroom" / "scene.json");
        CHECK(showroom->assetRoot == paths.content / "showroom" / "assets");
        CHECK(showroom->gbuffer);
        CHECK(showroom->bindlessMaterials);
        CHECK(showroom->staticGpuDriven);
        CHECK(showroom->hdrLighting);
        CHECK(showroom->ibl == ar::render::EnvironmentPreset::Directional);
        CHECK_FALSE(showroom->descriptorHeap);
        CHECK(showroom->output == paths.output);

        const auto showroomHeap = parseArgs(
            {L"--showroom",
             L"--descriptor-heap",
             L"--hidden",
             L"--frames",
             L"2",
             L"--exposure-ev",
             L"-0.5",
             L"--indirect-capacity",
             L"32",
             L"--screenshot",
             L"out/frame.png",
             L"--gbuffer-probes",
             L"out/probes.json",
             L"--output",
             L"custom_out"},
            paths);
        REQUIRE(showroomHeap.has_value());
        CHECK(showroomHeap->showroom);
        CHECK(showroomHeap->descriptorHeap);
        CHECK_FALSE(showroomHeap->visible);
        CHECK(showroomHeap->frames == 2u);
        REQUIRE(showroomHeap->exposureEV.has_value());
        CHECK(*showroomHeap->exposureEV == Catch::Approx(-0.5F));
        CHECK(showroomHeap->indirectCapacity == 32u);
        CHECK(showroomHeap->ibl == ar::render::EnvironmentPreset::Directional);
        CHECK(showroomHeap->screenshot == fs::path(L"out/frame.png"));
        CHECK(showroomHeap->gbufferProbes == fs::path(L"out/probes.json"));
        CHECK(showroomHeap->output == fs::path(L"custom_out"));

        const auto showroomConstantIbl = parseArgs({L"--showroom", L"--ibl", L"constant"}, paths);
        REQUIRE(showroomConstantIbl.has_value());
        CHECK(showroomConstantIbl->ibl == ar::render::EnvironmentPreset::Constant);

        // P4b shadow & free-camera switches
        const auto showroomShadowsFree =
            parseArgs({L"--showroom", L"--shadows", L"--free-camera"}, paths);
        REQUIRE(showroomShadowsFree.has_value());
        CHECK(showroomShadowsFree->shadows);
        CHECK_FALSE(showroomShadowsFree->shadowResolution.has_value());
        CHECK(showroomShadowsFree->freeCamera);
        CHECK(showroomShadowsFree->visible);

        const auto showroomShadowsHeap = parseArgs(
            {L"--showroom",
             L"--shadows",
             L"--shadow-resolution",
             L"1024",
             L"--descriptor-heap",
             L"--hidden",
             L"--frames",
             L"2"},
            paths);
        REQUIRE(showroomShadowsHeap.has_value());
        CHECK(showroomShadowsHeap->shadows);
        CHECK(showroomShadowsHeap->shadowResolution == 1024u);
        CHECK(showroomShadowsHeap->descriptorHeap);
        CHECK_FALSE(showroomShadowsHeap->freeCamera);

        const auto explicitShadows = parseArgs(
            {L"--static-scene",
             L"custom.json",
             L"--asset-root",
             L"custom_assets",
             L"--hdr-lighting",
             L"--shadows",
             L"--shadow-resolution",
             L"512",
             L"--hidden",
             L"--frames",
             L"2"},
            paths);
        REQUIRE(explicitShadows.has_value());
        CHECK(explicitShadows->shadows);
        CHECK(explicitShadows->shadowResolution == 512u);

        CHECK_FALSE(parseArgs(
            {L"--showroom", L"--free-camera", L"--hidden", L"--frames", L"2"}, paths));
        CHECK_FALSE(parseArgs({L"--free-camera"}, paths));
        CHECK_FALSE(parseArgs({L"--shadows"}, paths));
        CHECK_FALSE(parseArgs(
            {L"--static-scene",
             L"s.json",
             L"--asset-root",
             L"a",
             L"--gbuffer",
             L"--shadows",
             L"--hidden",
             L"--frames",
             L"2"},
            paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--shadow-resolution", L"1024"}, paths));
        CHECK_FALSE(
            parseArgs({L"--showroom", L"--shadows", L"--shadow-resolution", L"32"}, paths));
        CHECK_FALSE(
            parseArgs({L"--showroom", L"--shadows", L"--shadow-resolution", L"8192"}, paths));
        CHECK_FALSE(
            parseArgs({L"--showroom", L"--shadows", L"--shadow-resolution", L"abc"}, paths));
        CHECK_FALSE(parseArgs(
            {L"--showroom",
             L"--shadows",
             L"--shadow-resolution",
             L"512",
             L"--shadow-resolution",
             L"1024"},
            paths));

        // Strict conflict matrix with --showroom
        CHECK_FALSE(parseArgs(
            {L"--showroom", L"--static-scene", L"s.json", L"--asset-root", L"assets"}, paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--static-scene", L"s.json"}, paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--asset-root", L"assets"}, paths));
        CHECK_FALSE(parseArgs(
            {L"--showroom",
             L"--static-scene-test",
             L"alt.json",
             L"--screenshot",
             L"a.png",
             L"--frames",
             L"4"},
            paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--clear-only"}, paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--set-binding"}, paths));
        CHECK_FALSE(
            parseArgs({L"--showroom", L"--set-binding-retirement", L"--frames", L"3"}, paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--gpu-driven"}, paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--gpu-driven-empty"}, paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--hidden"}, paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--screenshot", L"out.png"}, paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--output", L""}, paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--frames", L"0"}, paths));
        CHECK_FALSE(parseArgs({L"--showroom", L"--frames", L"99999999999999999999"}, paths));

        ar::app::AppPaths emptyContent = paths;
        emptyContent.content.clear();
        CHECK_FALSE(parseArgs({L"--showroom"}, emptyContent));

        ar::app::AppPaths emptyOutput = paths;
        emptyOutput.output.clear();
        CHECK_FALSE(parseArgs({L"--showroom"}, emptyOutput));

        // Legacy invocation compatibility
        const auto bare = parseArgs({}, paths);
        REQUIRE(bare.has_value());
        CHECK_FALSE(bare->showroom);
        CHECK(bare->visible);
        CHECK_FALSE(bare->scene.has_value());
        CHECK(bare->output == paths.output);

        const auto clear = parseArgs({L"--clear-only", L"--hidden", L"--frames", L"2"}, paths);
        REQUIRE(clear.has_value());
        CHECK(clear->clearOnly);
        CHECK_FALSE(clear->showroom);

        const auto setBinding =
            parseArgs({L"--set-binding", L"--hidden", L"--frames", L"2"}, paths);
        REQUIRE(setBinding.has_value());
        CHECK(setBinding->setBinding);

        const auto gpuDriven = parseArgs({L"--gpu-driven", L"--hidden", L"--frames", L"2"}, paths);
        REQUIRE(gpuDriven.has_value());
        CHECK(gpuDriven->gpuDriven);
        CHECK_FALSE(gpuDriven->gpuEmpty);

        const auto explicitScene = parseArgs(
            {L"--static-scene",
             L"custom.json",
             L"--asset-root",
             L"custom_assets",
             L"--descriptor-heap",
             L"--hdr-lighting",
             L"--ibl",
             L"directional",
             L"--hidden",
             L"--frames",
             L"2"},
            paths);
        REQUIRE(explicitScene.has_value());
        CHECK_FALSE(explicitScene->showroom);
        CHECK(explicitScene->descriptorHeap);
        CHECK(explicitScene->scene == fs::path(L"custom.json"));
        CHECK(explicitScene->assetRoot == fs::path(L"custom_assets"));

        CHECK_FALSE(parseArgs({L"--static-scene", L"custom.json"}, paths));
        CHECK(ar::app::launchUsageText().find("--showroom") != std::string_view::npos);
        CHECK(ar::app::launchUsageText().find("--shadows") != std::string_view::npos);
        CHECK(ar::app::launchUsageText().find("--shadow-resolution") != std::string_view::npos);
        CHECK(ar::app::launchUsageText().find("--free-camera") != std::string_view::npos);
    }

    SECTION("Showroom scene deploys cleanly and passes full asset and render packing checks") {
        const fs::path sourceDir(AR_SHOWROOM_SOURCE_DIR);
        const fs::path deployedDir(AR_SHOWROOM_DEPLOYED_DIR);
        const fs::path deployedShaders(AR_TRIANGLE_DEPLOYED_SHADER_DIR);

        REQUIRE(ar::app::requireReadableFile(sourceDir / "LICENSE.txt", "showroom license"));
        REQUIRE(ar::app::requireReadableFile(sourceDir / "scene.json", "showroom source scene"));
        REQUIRE(
            ar::app::requireReadableFile(
                sourceDir / "generate_assets.py", "showroom asset generator"));
        REQUIRE(ar::app::requireReadableFile(deployedDir / "LICENSE.txt", "deployed license"));
        REQUIRE(ar::app::requireReadableFile(deployedDir / "scene.json", "deployed scene"));
        REQUIRE(
            ar::app::requireReadableFile(
                deployedShaders / "static_indirect.comp.spv", "deployed compute shader"));
        REQUIRE(
            ar::app::requireReadableFile(
                deployedShaders / "deferred_ibl.frag.spv", "deployed IBL shader"));

        auto readBytes = [](const fs::path& file) {
            std::ifstream stream(file, std::ios::binary);
            REQUIRE(stream.good());
            return std::string(
                std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        };
        CHECK(readBytes(sourceDir / "scene.json") == readBytes(deployedDir / "scene.json"));
        CHECK(readBytes(sourceDir / "LICENSE.txt") == readBytes(deployedDir / "LICENSE.txt"));

        ar::runtime::StaticSceneSession session(deployedDir / "assets", true);
        const auto opened = session.open(deployedDir / "scene.json");
        REQUIRE(opened.has_value());

        const auto input = session.input();
        REQUIRE(input != nullptr);
        REQUIRE(input->draws.size() == 14u);

        std::map<std::uint64_t, const ar::render::StaticDraw*> byId;
        for (const auto& draw : input->draws) {
            REQUIRE(draw.mesh != nullptr);
            CHECK_FALSE(draw.mesh->vertices.empty());
            CHECK_FALSE(draw.mesh->indices.empty());
            CHECK(draw.mesh->indices.size() % 3u == 0u);
            CHECK(draw.mesh->normals.size() == draw.mesh->vertices.size());
            CHECK(draw.mesh->width > 0u);
            CHECK(draw.mesh->height > 0u);
            CHECK_FALSE(draw.mesh->rgba.empty());
            REQUIRE(byId.emplace(draw.objectId, &draw).second);
        }

        const std::array<std::uint64_t, 14> expectedIds{
            1u, 2u, 3u, 4u, 101u, 102u, 103u, 104u, 105u, 201u, 202u, 203u, 204u, 205u};
        for (const auto id : expectedIds) {
            REQUIRE(byId.contains(id));
        }

        // Instances 3 and 4 share pillar.gltf and must reuse the cached StaticMesh instance.
        CHECK(byId.at(3u)->mesh == byId.at(4u)->mesh);

        // Roughness progression: IDs 101..105 have metallic == 0.0 and strictly increasing
        // roughness.
        const std::array<float, 5> expectedRoughness{0.10F, 0.30F, 0.50F, 0.70F, 0.90F};
        for (std::size_t i = 0; i < expectedRoughness.size(); ++i) {
            const auto& mat = byId.at(101u + i)->mesh->material;
            CHECK(mat.metallicFactor == Catch::Approx(0.0F));
            CHECK(mat.roughnessFactor == Catch::Approx(expectedRoughness[i]));
        }

        // Metallic progression: IDs 201..205 have roughness == 0.25 and strictly increasing
        // metallic.
        const std::array<float, 5> expectedMetallic{0.00F, 0.25F, 0.50F, 0.75F, 1.00F};
        for (std::size_t i = 0; i < expectedMetallic.size(); ++i) {
            const auto& mat = byId.at(201u + i)->mesh->material;
            CHECK(mat.roughnessFactor == Catch::Approx(0.25F));
            CHECK(mat.metallicFactor == Catch::Approx(expectedMetallic[i]));
        }

        // End-to-end CPU geometry & indirect draw-data packing succeeds with mesh deduplication.
        const auto packed = ar::render::packStaticSceneDrawData(*input);
        REQUIRE(packed.has_value());
        CHECK(packed->instances.size() == 14u);
        CHECK(packed->meshes.size() == 13u);
        CHECK(packed->materials.size() == 13u);

        // Scene save round-trip preserves camera and all instances.
        const TempDir temp(testOutputRoot());
        const fs::path savedPath = temp.path / "showroom_roundtrip.json";
        REQUIRE(session.save(savedPath).has_value());
        ar::runtime::StaticSceneSession reloadedSession(deployedDir / "assets", true);
        REQUIRE(reloadedSession.open(savedPath).has_value());
        const auto reloaded = reloadedSession.input();
        REQUIRE(reloaded != nullptr);
        CHECK(reloaded->camera == input->camera);
        CHECK(reloaded->draws.size() == input->draws.size());
    }
}
