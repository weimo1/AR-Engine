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

    SECTION("parseLaunchOptions defaults zero-arg launch to showroom and rejects legacy flags") {
        const ar::app::AppPaths paths{
            fs::path(u8"C:/ar-app/shaders"),
            fs::path(u8"C:/ar-app/content"),
            fs::path(u8"C:/ar-app/output"),
        };

        // Zero-arg launch opens the built-in showroom interactively with zero CLI flags.
        const auto zeroArg = parseArgs({}, paths);
        REQUIRE(zeroArg.has_value());
        CHECK(zeroArg->visible);
        CHECK_FALSE(zeroArg->descriptorHeap);
        CHECK_FALSE(zeroArg->frames.has_value());
        CHECK(zeroArg->scene == paths.content / "showroom" / "scene.json");
        CHECK(zeroArg->assetRoot == paths.content / "showroom" / "assets");
        CHECK_FALSE(zeroArg->screenshot.has_value());
        CHECK_FALSE(zeroArg->gbufferProbes.has_value());
        CHECK(zeroArg->output == paths.output);

        // Minimal automation / parity overrides
        const auto heapCapture = parseArgs(
            {L"--descriptor-heap",
             L"--hidden",
             L"--frames",
             L"2",
             L"--screenshot",
             L"out/frame.png",
             L"--gbuffer-probes",
             L"out/probes.json",
             L"--output",
             L"custom_out"},
            paths);
        REQUIRE(heapCapture.has_value());
        CHECK(heapCapture->descriptorHeap);
        CHECK_FALSE(heapCapture->visible);
        CHECK(heapCapture->frames == 2u);
        CHECK(heapCapture->scene == paths.content / "showroom" / "scene.json");
        CHECK(heapCapture->assetRoot == paths.content / "showroom" / "assets");
        CHECK(heapCapture->screenshot == fs::path(L"out/frame.png"));
        CHECK(heapCapture->gbufferProbes == fs::path(L"out/probes.json"));
        CHECK(heapCapture->output == fs::path(L"custom_out"));

        // Explicit custom scene + asset root override
        const auto customScene = parseArgs(
            {L"--scene",
             L"custom.json",
             L"--asset-root",
             L"custom_assets",
             L"--descriptor-heap",
             L"--hidden",
             L"--frames",
             L"2"},
            paths);
        REQUIRE(customScene.has_value());
        CHECK(customScene->descriptorHeap);
        CHECK(customScene->scene == fs::path(L"custom.json"));
        CHECK(customScene->assetRoot == fs::path(L"custom_assets"));

        // Legacy P1-P5 mode flags are removed from ar_engine and rejected with exit 2.
        CHECK_FALSE(parseArgs({L"--showroom"}, paths));
        CHECK_FALSE(parseArgs({L"--shadows"}, paths));
        CHECK_FALSE(parseArgs({L"--free-camera"}, paths));
        CHECK_FALSE(parseArgs({L"--shadow-resolution", L"1024"}, paths));
        CHECK_FALSE(parseArgs({L"--clear-only"}, paths));
        CHECK_FALSE(parseArgs({L"--set-binding"}, paths));
        CHECK_FALSE(parseArgs({L"--gpu-driven"}, paths));
        CHECK_FALSE(parseArgs({L"--gbuffer"}, paths));
        CHECK_FALSE(parseArgs({L"--hdr-lighting"}, paths));
        CHECK_FALSE(parseArgs({L"--ibl", L"directional"}, paths));
        CHECK_FALSE(parseArgs({L"--static-scene", L"s.json", L"--asset-root", L"assets"}, paths));

        // Validation rules on remaining minimal flags
        CHECK_FALSE(parseArgs({L"--scene", L"s.json"}, paths));
        CHECK_FALSE(parseArgs({L"--asset-root", L"assets"}, paths));
        CHECK_FALSE(parseArgs({L"--scene", L"", L"--asset-root", L"assets"}, paths));
        CHECK_FALSE(parseArgs({L"--scene", L"s.json", L"--asset-root", L""}, paths));
        CHECK_FALSE(parseArgs({L"--hidden"}, paths));
        CHECK_FALSE(parseArgs({L"--screenshot", L"out.png"}, paths));
        CHECK_FALSE(parseArgs({L"--gbuffer-probes", L"out.json"}, paths));
        CHECK_FALSE(parseArgs({L"--output", L""}, paths));
        CHECK_FALSE(parseArgs({L"--frames", L"0"}, paths));
        CHECK_FALSE(parseArgs({L"--frames", L"99999999999999999999"}, paths));
        CHECK_FALSE(parseArgs({L"--frames", L"abc"}, paths));
        CHECK_FALSE(parseArgs({L"--frames", L"2", L"--frames", L"3"}, paths));

        ar::app::AppPaths emptyContent = paths;
        emptyContent.content.clear();
        CHECK_FALSE(parseArgs({}, emptyContent));
        // Custom scene override does not depend on built-in showroom content path.
        CHECK(parseArgs({L"--scene", L"s.json", L"--asset-root", L"assets"}, emptyContent)
                  .has_value());

        ar::app::AppPaths emptyOutput = paths;
        emptyOutput.output.clear();
        CHECK_FALSE(parseArgs({}, emptyOutput));

        // Benchmark automation entry: standalone --benchmark SPEC_JSON only.
        const auto bench = parseArgs({L"--benchmark", L"bench/spec.json"}, paths);
        REQUIRE(bench.has_value());
        CHECK(bench->benchmarkSpec == fs::path(L"bench/spec.json"));
        CHECK_FALSE(parseArgs({L"--benchmark", L""}, paths));
        CHECK_FALSE(parseArgs({L"--benchmark", L"s.json", L"--descriptor-heap"}, paths));
        CHECK_FALSE(parseArgs({L"--benchmark", L"s.json", L"--hidden", L"--frames", L"2"}, paths));

        CHECK(ar::app::launchUsageText().find("ar_engine") != std::string_view::npos);
        CHECK(ar::app::launchUsageText().find("--descriptor-heap") != std::string_view::npos);
        CHECK(ar::app::launchUsageText().find("--benchmark") != std::string_view::npos);
        CHECK(ar::app::launchUsageText().find("--showroom") == std::string_view::npos);
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
