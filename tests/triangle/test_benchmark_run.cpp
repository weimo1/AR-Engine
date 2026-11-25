#include "BenchmarkRun.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

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
              ("bench-unit-" +
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
    "BenchmarkRun parses specs, computes exact quantiles, joins CPU/GPU strictly by frameNumber, "
    "isolates overflow samples, and exports parseable CSV + JSON",
    "[app-benchmark-run]") {
    using namespace ar;
    const TempDir temp(testOutputRoot());
    const app::AppPaths paths{
        temp.path / "shaders", temp.path / "content", temp.path / "default-out"};

    SECTION("loadBenchmarkConfig validates required fields and rejects bad configs") {
        const fs::path specPath = temp.path / "valid.json";
        {
            std::ofstream out(specPath, std::ios::binary);
            out << Json{{"version", 1}, {"name", "medium-heap-gpu"}, {"preset_version", "p6-v1"}, {"seed", 42}, {"binding", "descriptor_heap"}, {"cull", "gpu"}, {"present_mode", "immediate"}, {"width", 1280}, {"height", 720}, {"shadows", true}, {"shadow_resolution", 2048}, {"exposure_ev", 0.5}, {"warmup_frames", 8}, {"sample_frames", 60}, {"repeats", 2}, {"output_dir", "out_run"}}
                       .dump(2);
        }
        const auto loaded = app::loadBenchmarkConfig(specPath, paths);
        REQUIRE(loaded.has_value());
        CHECK(loaded->name == "medium-heap-gpu");
        CHECK(loaded->seed == 42u);
        CHECK(loaded->descriptorHeap);
        CHECK(loaded->cullMode == rhi::StaticCullMode::GpuCompute);
        CHECK(loaded->presentMode == rhi::PresentMode::Immediate);
        CHECK(loaded->scene == paths.content / "showroom" / "scene.json");
        CHECK(loaded->assetRoot == paths.content / "showroom" / "assets");
        CHECK(loaded->outputDir == temp.path / "out_run");
        CHECK(loaded->capacity == UINT32_MAX);

        // Reject invalid cull mode, invalid version, missing output_dir, half scene override.
        const auto writeBad = [&](const std::string& filename, const Json& j) {
            const fs::path p = temp.path / filename;
            std::ofstream out(p, std::ios::binary);
            out << j.dump();
            return p;
        };
        CHECK_FALSE(
            app::loadBenchmarkConfig(
                writeBad("bad_ver.json", {{"version", 2}, {"name", "a"}, {"output_dir", "o"}}),
                paths)
                .has_value());
        CHECK_FALSE(
            app::loadBenchmarkConfig(
                writeBad(
                    "bad_cull.json",
                    {{"version", 1}, {"name", "a"}, {"cull", "magic"}, {"output_dir", "o"}}),
                paths)
                .has_value());
        CHECK_FALSE(
            app::loadBenchmarkConfig(
                writeBad(
                    "half_scene.json",
                    {{"version", 1}, {"name", "a"}, {"scene", "s.json"}, {"output_dir", "o"}}),
                paths)
                .has_value());
        CHECK_FALSE(
            app::loadBenchmarkConfig(
                writeBad("no_out.json", {{"version", 1}, {"name", "a"}}), paths)
                .has_value());
    }

    SECTION("evaluateBenchmarkCamera produces unit quaternions and sweeps across frames") {
        render::StaticCamera base{};
        base.position = {0.F, 4.F, 12.F};
        base.orientation = {0.F, 0.F, 0.F, 1.F};
        const auto c0 = app::evaluateBenchmarkCamera(base, 0u, 100u);
        const auto c25 = app::evaluateBenchmarkCamera(base, 25u, 100u);
        const auto c75 = app::evaluateBenchmarkCamera(base, 75u, 100u);
        const auto normSq = [](const std::array<float, 4>& q) {
            return q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        };
        CHECK(normSq(c0.orientation) == Catch::Approx(1.F).margin(1e-5F));
        CHECK(normSq(c25.orientation) == Catch::Approx(1.F).margin(1e-5F));
        CHECK(normSq(c75.orientation) == Catch::Approx(1.F).margin(1e-5F));
        CHECK(c25.orientation != c75.orientation);
        CHECK(c25.position[0] > base.position[0] + 0.5F);
        CHECK(c75.position[0] < base.position[0] - 0.5F);
    }

    SECTION("computeMetricStats calculates exact median, P95, min, max, mean") {
        const std::vector<double> values{10.0, 2.0, 4.0, 8.0, 6.0, 12.0, 14.0, 16.0, 18.0, 20.0};
        const auto stats = app::computeMetricStats(values, 2u);
        CHECK(stats.sampleCount == 10u);
        CHECK(stats.missingCount == 2u);
        REQUIRE(stats.minMs.has_value());
        REQUIRE(stats.medianMs.has_value());
        REQUIRE(stats.p95Ms.has_value());
        REQUIRE(stats.maxMs.has_value());
        REQUIRE(stats.meanMs.has_value());
        CHECK(*stats.minMs == Catch::Approx(2.0));
        CHECK(*stats.maxMs == Catch::Approx(20.0));
        CHECK(*stats.medianMs == Catch::Approx(11.0));
        // 0.95 * (10 - 1) = 8.55 => 18.0 * 0.45 + 20.0 * 0.55 = 19.1
        CHECK(*stats.p95Ms == Catch::Approx(19.1));
        CHECK(*stats.meanMs == Catch::Approx(11.0));
    }

    SECTION("joinBenchmarkSamples joins strictly by frameNumber and isolates overflowed samples") {
        std::vector<app::BenchmarkCpuSample> cpuSamples{
            {0u, 0u, rhi::CpuFrameTimings{10u, 0.25, 0.50, 0.75, 100u, 100u, false}},
            {0u, 1u, rhi::CpuFrameTimings{11u, 0.35, 0.60, 0.95, 110u, 110u, false}},
            // Frame 12 has missing GPU sample; must NOT pull frame 99!
            {0u, 2u, rhi::CpuFrameTimings{12u, 0.45, 0.70, 1.15, 120u, 120u, false}},
            // Frame 13 overflowed; must be excluded from formal stats and counted separately!
            {0u, 3u, rhi::CpuFrameTimings{13u, 9.99, 9.99, 19.98, 500u, 128u, true}},
        };
        std::vector<rhi::GpuFrameTimings> gpuSamples{
            rhi::GpuFrameTimings{
                99u, true, {}, 0.1, std::nullopt, 1.0, 0.2, 0.1, std::nullopt, 1.4},
            rhi::GpuFrameTimings{
                11u, true, {}, 0.2, std::nullopt, 2.0, 0.4, 0.2, std::nullopt, 2.8},
            rhi::GpuFrameTimings{
                10u, true, {}, 0.1, std::nullopt, 1.0, 0.3, 0.1, std::nullopt, 1.5},
            rhi::GpuFrameTimings{
                13u, true, {}, 0.5, std::nullopt, 5.0, 0.5, 0.5, std::nullopt, 6.5},
        };

        const auto joined =
            app::joinBenchmarkSamples(cpuSamples, gpuSamples, rhi::StaticCullMode::CpuFrustum);
        REQUIRE(joined.size() == 4u);
        CHECK(joined[0].frameNumber == 10u);
        CHECK(joined[0].gpuAvailable);
        CHECK(joined[0].gpuTotalMs == Catch::Approx(1.5));
        CHECK(joined[1].frameNumber == 11u);
        CHECK(joined[1].gpuAvailable);
        CHECK(joined[1].gpuTotalMs == Catch::Approx(2.8));
        CHECK(joined[2].frameNumber == 12u);
        CHECK_FALSE(joined[2].gpuAvailable);
        CHECK_FALSE(joined[2].gpuUnavailableReason.empty());
        CHECK_FALSE(joined[2].gpuTotalMs.has_value());
        CHECK(joined[3].overflowed);

        app::BenchmarkConfig cfg{};
        cfg.name = "unit-export";
        cfg.cullMode = rhi::StaticCullMode::CpuFrustum;
        cfg.outputDir = temp.path / "exported";
        app::BenchmarkSummaryMetadata meta{};
        meta.deviceName = "TestGPU";
        meta.deviceApi = "1.4.0";
        meta.buildConfiguration = "Debug";
        meta.buildIdentity = "abc123";
        meta.instanceCount = 256u;
        meta.activePresentMode = rhi::PresentMode::Immediate;

        REQUIRE(app::writeBenchmarkOutputs(cfg, meta, joined).has_value());
        REQUIRE(fs::is_regular_file(cfg.outputDir / "frames.csv"));
        REQUIRE(fs::is_regular_file(cfg.outputDir / "summary.json"));

        std::ifstream summaryStream(cfg.outputDir / "summary.json");
        Json summary;
        summaryStream >> summary;
        CHECK(summary.at("status") == "completed");
        CHECK(summary.at("total_recorded_samples") == 4u);
        CHECK(summary.at("formal_non_overflow_samples") == 3u);
        CHECK(summary.at("overflow_sample_count") == 1u);
        CHECK(summary.at("metrics").at("cpu_cull_ms").at("sample_count") == 3u);
        CHECK(
            summary.at("metrics").at("cpu_cull_ms").at("median_ms").get<double>() ==
            Catch::Approx(0.35));
        CHECK(summary.at("metrics").at("gpu_total_ms").at("sample_count") == 2u);
        CHECK(summary.at("metrics").at("gpu_total_ms").at("missing_count") == 1u);
        CHECK(
            summary.at("metrics").at("gpu_total_ms").at("median_ms").get<double>() ==
            Catch::Approx(2.15));
        CHECK(summary.at("metrics").at("gpu_cull_ms").at("sample_count") == 0u);
    }
}
