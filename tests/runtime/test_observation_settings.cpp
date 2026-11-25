#include "runtime/ObservationSettings.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include "render/DirectionalShadow.h"

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
              ("obs-preset-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

ar::runtime::ObservationSettings sampleSettings() {
    ar::runtime::ObservationSettings settings{};
    settings.version = ar::runtime::kObservationSettingsVersion;
    settings.name = "pillar-shadow-check";
    settings.sceneRef = "showroom/scene.json";
    settings.camera.position = {-1.8F, 1.9F, 2.4F};
    settings.camera.orientation = {-0.0985376F, 0.F, 0.F, 0.9951333F};
    settings.camera.verticalFovRadians = 0.7853982F;
    settings.camera.nearPlane = 0.1F;
    settings.camera.farPlane = 50.F;
    settings.exposureEV = -0.75F;
    settings.directionalLight = ar::render::defaultDirectionalLight();
    settings.directionalLight.intensity = 4.25F;
    settings.directionalLight.castShadows = true;
    settings.viewMode = ar::rhi::DebugViewMode::ViewNormal;
    return settings;
}

} // namespace

TEST_CASE(
    "ObservationSettings round-trips all 6 view modes and rejects corrupt or mismatched presets",
    "[runtime-observation-settings]") {
    using ar::rhi::DebugViewMode;
    using ar::runtime::debugViewModeName;
    using ar::runtime::loadObservationSettings;
    using ar::runtime::ObservationSettings;
    using ar::runtime::parseDebugViewMode;
    using ar::runtime::saveObservationSettings;

    const TempDir temp(testOutputRoot());

    SECTION("all 6 debug view modes map bijectively to stable names") {
        constexpr DebugViewMode modes[] = {
            DebugViewMode::Final,
            DebugViewMode::BaseColor,
            DebugViewMode::ViewNormal,
            DebugViewMode::MetallicRoughness,
            DebugViewMode::MainDepth,
            DebugViewMode::ShadowDepth,
        };
        for (const auto mode : modes) {
            const auto name = debugViewModeName(mode);
            REQUIRE(name != "unknown");
            const auto parsed = parseDebugViewMode(name);
            REQUIRE(parsed.has_value());
            CHECK(*parsed == mode);
        }
        CHECK_FALSE(parseDebugViewMode("bogus").has_value());
    }

    SECTION("valid preset saves atomically and loads back identically") {
        const fs::path presetPath = temp.path / "showroom_view.json";
        const ObservationSettings expected = sampleSettings();
        REQUIRE(saveObservationSettings(presetPath, expected).has_value());

        const auto loaded = loadObservationSettings(presetPath, "showroom/scene.json");
        INFO((loaded ? "" : loaded.error().message));
        REQUIRE(loaded.has_value());
        CHECK(loaded->version == expected.version);
        CHECK(loaded->name == expected.name);
        CHECK(loaded->sceneRef == expected.sceneRef);
        CHECK(loaded->exposureEV == Catch::Approx(expected.exposureEV));
        CHECK(loaded->viewMode == expected.viewMode);
        CHECK(loaded->directionalLight.castShadows == expected.directionalLight.castShadows);
        CHECK(
            loaded->directionalLight.intensity ==
            Catch::Approx(expected.directionalLight.intensity));
        for (std::size_t i = 0; i < 3u; ++i) {
            CHECK(loaded->camera.position[i] == Catch::Approx(expected.camera.position[i]));
            CHECK(
                loaded->directionalLight.direction[i] ==
                Catch::Approx(expected.directionalLight.direction[i]));
            CHECK(
                loaded->directionalLight.color[i] ==
                Catch::Approx(expected.directionalLight.color[i]));
        }
        for (std::size_t i = 0; i < 4u; ++i) {
            CHECK(loaded->camera.orientation[i] == Catch::Approx(expected.camera.orientation[i]));
        }

        // Mismatched scene_ref is rejected with clear file + reason diagnostic.
        const auto wrongScene = loadObservationSettings(presetPath, "other/scene.json");
        REQUIRE_FALSE(wrongScene.has_value());
        CHECK(wrongScene.error().category == ar::core::ErrorCategory::InvalidArguments);
        CHECK(wrongScene.error().message.find("showroom_view.json") != std::string::npos);
        CHECK(wrongScene.error().message.find("scene_ref") != std::string::npos);
    }

    SECTION("corrupt files, unsupported versions, and invalid fields fail without mutating files") {
        const fs::path validPath = temp.path / "keep_me.json";
        const ObservationSettings valid = sampleSettings();
        REQUIRE(saveObservationSettings(validPath, valid).has_value());

        // Attempting to overwrite with invalid exposure must fail before touching disk.
        ObservationSettings badExposure = valid;
        badExposure.exposureEV = 24.F;
        const auto badSave = saveObservationSettings(validPath, badExposure);
        REQUIRE_FALSE(badSave.has_value());
        const auto stillGood = loadObservationSettings(validPath, "showroom/scene.json");
        REQUIRE(stillGood.has_value());
        CHECK(stillGood->exposureEV == Catch::Approx(valid.exposureEV));

        // Attempting to save into a directory path fails cleanly without leaving temp files.
        const fs::path dirTarget = temp.path / "as_dir";
        fs::create_directories(dirTarget);
        CHECK_FALSE(saveObservationSettings(dirTarget, valid).has_value());

        // Reject invalid scene refs, non-unit quaternion, NaN exposure.
        ObservationSettings badScene = valid;
        badScene.sceneRef = "../escape/scene.json";
        CHECK_FALSE(saveObservationSettings(temp.path / "bad_scene.json", badScene).has_value());

        ObservationSettings badQuat = valid;
        badQuat.camera.orientation = {0.F, 0.F, 0.F, 0.F};
        CHECK_FALSE(saveObservationSettings(temp.path / "bad_quat.json", badQuat).has_value());

        ObservationSettings nanExp = valid;
        nanExp.exposureEV = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(saveObservationSettings(temp.path / "bad_nan.json", nanExp).has_value());

        // Missing file reports path.
        const auto missing = loadObservationSettings(temp.path / "does_not_exist.json");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().message.find("does_not_exist.json") != std::string::npos);

        // Corrupt JSON file reports path and reason.
        const fs::path corruptPath = temp.path / "corrupt.json";
        {
            std::ofstream out(corruptPath, std::ios::binary);
            out << "{not valid json";
        }
        const auto corrupt = loadObservationSettings(corruptPath);
        REQUIRE_FALSE(corrupt.has_value());
        CHECK(corrupt.error().message.find("corrupt.json") != std::string::npos);

        // Unsupported version file is rejected.
        const fs::path wrongVerPath = temp.path / "v99.json";
        {
            std::ofstream out(wrongVerPath, std::ios::binary);
            out << R"({"version": 99, "scene_ref": "showroom/scene.json"})";
        }
        const auto wrongVer = loadObservationSettings(wrongVerPath);
        REQUIRE_FALSE(wrongVer.has_value());
        CHECK(wrongVer.error().message.find("99") != std::string::npos);
    }
}
