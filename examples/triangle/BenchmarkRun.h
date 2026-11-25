#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "AppPaths.h"
#include "render/StaticRenderInput.h"
#include "rhi/FrameTimings.h"
#include "rhi/RhiSession.h"
#include "rhi/StaticSceneDraw.h"

namespace ar::app {

inline constexpr std::uint32_t kBenchmarkSpecVersion = 1u;

struct BenchmarkConfig {
    std::uint32_t version = kBenchmarkSpecVersion;
    std::string name;
    std::string presetVersion = "p6-bench-v1";
    std::uint64_t seed = 0u;
    std::filesystem::path scene;
    std::filesystem::path assetRoot;
    bool descriptorHeap = false;
    rhi::StaticCullMode cullMode = rhi::StaticCullMode::GpuCompute;
    rhi::PresentMode presentMode = rhi::PresentMode::Immediate;
    std::uint32_t width = 1280u;
    std::uint32_t height = 720u;
    bool shadows = true;
    std::uint32_t shadowResolution = 2048u;
    float exposureEV = 0.F;
    std::uint32_t warmupFrames = 16u;
    std::uint32_t sampleFrames = 120u;
    std::uint32_t repeats = 1u;
    std::uint32_t capacity = UINT32_MAX;
    bool hidden = true;
    std::filesystem::path outputDir;
};

struct BenchmarkMetricStats {
    std::size_t sampleCount = 0u;
    std::size_t missingCount = 0u;
    std::optional<double> minMs;
    std::optional<double> medianMs;
    std::optional<double> p95Ms;
    std::optional<double> maxMs;
    std::optional<double> meanMs;
};

struct BenchmarkCpuSample {
    std::uint32_t repeatIndex = 0u;
    std::uint32_t sampleIndex = 0u;
    rhi::CpuFrameTimings timings{};
};

struct BenchmarkFrameRecord {
    std::uint32_t repeatIndex = 0u;
    std::uint32_t sampleIndex = 0u;
    std::uint64_t frameNumber = 0u;
    std::optional<std::uint32_t> visibleCount;
    std::optional<std::uint32_t> drawCount;
    bool overflowed = false;
    std::optional<double> cpuCullMs;
    double cpuRecordSubmitMs = 0.0;
    double cpuTotalMs = 0.0;
    bool gpuAvailable = false;
    std::string gpuUnavailableReason;
    std::optional<double> gpuCullMs;
    std::optional<double> gpuShadowMs;
    std::optional<double> gpuGeometryMs;
    std::optional<double> gpuLightingMs;
    std::optional<double> gpuDisplayMs;
    std::optional<double> gpuTotalMs;
};

struct BenchmarkSummaryMetadata {
    std::string status = "completed";
    std::string skipReason;
    std::string deviceName;
    std::string deviceApi;
    std::string buildConfiguration;
    std::string buildIdentity;
    std::uint32_t instanceCount = 0u;
    rhi::PresentMode activePresentMode = rhi::PresentMode::Fifo;
};

[[nodiscard]] std::expected<BenchmarkConfig, std::string>
loadBenchmarkConfig(const std::filesystem::path& specPath, const AppPaths& paths);

[[nodiscard]] render::StaticCamera evaluateBenchmarkCamera(
    const render::StaticCamera& baseCamera,
    std::uint32_t frameIndex,
    std::uint32_t totalFrames) noexcept;

[[nodiscard]] BenchmarkMetricStats
computeMetricStats(std::span<const double> validSamples, std::size_t missingCount = 0u);

[[nodiscard]] std::vector<BenchmarkFrameRecord> joinBenchmarkSamples(
    std::span<const BenchmarkCpuSample> cpuSamples,
    std::span<const rhi::GpuFrameTimings> gpuSamples,
    rhi::StaticCullMode cullMode);

[[nodiscard]] std::expected<void, std::string> writeBenchmarkOutputs(
    const BenchmarkConfig& config,
    const BenchmarkSummaryMetadata& metadata,
    std::span<const BenchmarkFrameRecord> records);

[[nodiscard]] int runBenchmark(const BenchmarkConfig& config, const AppPaths& paths);

} // namespace ar::app
