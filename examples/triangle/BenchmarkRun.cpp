#include "BenchmarkRun.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numbers>
#include <numeric>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/BuildInfo.h"
#include "render/EnvironmentLighting.h"
#include "runtime/RuntimeSession.h"

namespace ar::app {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

std::string pathToUtf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string_view presentModeName(rhi::PresentMode mode) noexcept {
    switch (mode) {
    case rhi::PresentMode::Fifo:
        return "fifo";
    case rhi::PresentMode::Mailbox:
        return "mailbox";
    case rhi::PresentMode::Immediate:
        return "immediate";
    }
    return "fifo";
}

std::string_view cullModeName(rhi::StaticCullMode mode) noexcept {
    return mode == rhi::StaticCullMode::CpuFrustum ? "cpu" : "gpu";
}

std::string formatVulkanApi(std::uint32_t version) {
    const std::uint32_t major = (version >> 22) & 0x7Fu;
    const std::uint32_t minor = (version >> 12) & 0x3FFu;
    const std::uint32_t patch = version & 0xFFFu;
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

std::expected<void, std::string>
writeFileAtomic(const fs::path& destination, std::string_view data) {
    std::error_code err;
    if (destination.has_parent_path()) {
        fs::create_directories(destination.parent_path(), err);
        if (err) {
            return std::unexpected(
                "failed to create output directory: " + pathToUtf8(destination.parent_path()));
        }
    }
    const fs::path tempPath =
        destination.parent_path() /
        (destination.filename().string() + ".tmp." +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::unexpected("failed to open temp file for writing: " + pathToUtf8(tempPath));
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            fs::remove(tempPath, err);
            return std::unexpected("failed to write file bytes: " + pathToUtf8(destination));
        }
    }
    fs::rename(tempPath, destination, err);
    if (err) {
        std::error_code rmErr;
        fs::remove(destination, rmErr);
        err.clear();
        fs::rename(tempPath, destination, err);
    }
    if (err) {
        std::error_code rmErr;
        fs::remove(tempPath, rmErr);
        return std::unexpected(
            "failed to replace benchmark output file " + pathToUtf8(destination) + ": " +
            err.message());
    }
    return {};
}

std::optional<std::vector<std::uint32_t>> readShaderWords(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto bytes = static_cast<std::streamoff>(input.tellg());
    constexpr auto wordSize = static_cast<std::streamoff>(sizeof(std::uint32_t));
    constexpr std::streamoff maxShaderBytes = 1 << 20;
    if (bytes < 5 * wordSize || bytes > maxShaderBytes || bytes % wordSize != 0) {
        std::fwprintf(stderr, L"Missing or invalid shader: %ls\n", path.c_str());
        return std::nullopt;
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes / wordSize));
    if (!input.seekg(0).read(reinterpret_cast<char*>(words.data()), bytes)) {
        std::fwprintf(stderr, L"Could not read shader: %ls\n", path.c_str());
        return std::nullopt;
    }
    return words;
}

double linearQuantile(const std::vector<double>& sorted, double q) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1u) {
        return sorted.front();
    }
    const double pos = std::clamp(q, 0.0, 1.0) * static_cast<double>(sorted.size() - 1u);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = static_cast<std::size_t>(std::ceil(pos));
    const double frac = pos - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

Json metricToJson(const BenchmarkMetricStats& stats, const std::set<std::string>& reasons = {}) {
    Json j{
        {"unit", "ms"},
        {"sample_count", stats.sampleCount},
        {"missing_count", stats.missingCount},
    };
    if (stats.minMs) {
        j["min_ms"] = *stats.minMs;
    } else {
        j["min_ms"] = nullptr;
    }
    if (stats.medianMs) {
        j["median_ms"] = *stats.medianMs;
    } else {
        j["median_ms"] = nullptr;
    }
    if (stats.p95Ms) {
        j["p95_ms"] = *stats.p95Ms;
    } else {
        j["p95_ms"] = nullptr;
    }
    if (stats.maxMs) {
        j["max_ms"] = *stats.maxMs;
    } else {
        j["max_ms"] = nullptr;
    }
    if (stats.meanMs) {
        j["mean_ms"] = *stats.meanMs;
    } else {
        j["mean_ms"] = nullptr;
    }
    if (!reasons.empty()) {
        j["unavailable_reasons"] = Json::array();
        for (const auto& r : reasons) {
            j["unavailable_reasons"].push_back(r);
        }
    }
    return j;
}

std::array<float, 4>
mulQuat(const std::array<float, 4>& a, const std::array<float, 4>& b) noexcept {
    // [x, y, z, w]
    return {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
}

std::array<float, 4> normalizeQuat(std::array<float, 4> q) noexcept {
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len <= 1e-6F) {
        return {0.F, 0.F, 0.F, 1.F};
    }
    const float inv = 1.F / len;
    return {q[0] * inv, q[1] * inv, q[2] * inv, q[3] * inv};
}

fs::path resolveSpecPath(const std::string& raw, const fs::path& baseDir) {
    fs::path p(std::u8string(raw.begin(), raw.end()));
    if (p.is_relative() && !baseDir.empty()) {
        return baseDir / p;
    }
    return p;
}

} // namespace

std::expected<BenchmarkConfig, std::string>
loadBenchmarkConfig(const fs::path& specPath, const AppPaths& paths) {
    std::ifstream input(specPath, std::ios::binary);
    if (!input) {
        return std::unexpected("could not open benchmark spec: " + pathToUtf8(specPath));
    }
    Json doc;
    try {
        input >> doc;
    } catch (const Json::exception& e) {
        return std::unexpected(std::string("invalid benchmark spec JSON: ") + e.what());
    }
    if (!doc.is_object()) {
        return std::unexpected("benchmark spec root must be a JSON object");
    }

    BenchmarkConfig cfg{};
    const fs::path specDir = specPath.parent_path();
    try {
        if (!doc.contains("version") || !doc.at("version").is_number_unsigned() ||
            doc.at("version").get<std::uint32_t>() != kBenchmarkSpecVersion) {
            return std::unexpected("benchmark spec version must be 1");
        }
        if (!doc.contains("name") || !doc.at("name").is_string() ||
            doc.at("name").get<std::string>().empty()) {
            return std::unexpected("benchmark spec requires a non-empty 'name'");
        }
        cfg.name = doc.at("name").get<std::string>();
        if (doc.contains("preset_version")) {
            cfg.presetVersion = doc.at("preset_version").get<std::string>();
        }
        if (doc.contains("seed")) {
            cfg.seed = doc.at("seed").get<std::uint64_t>();
        }
        if (doc.contains("scene") && doc.contains("asset_root")) {
            cfg.scene = resolveSpecPath(doc.at("scene").get<std::string>(), specDir);
            cfg.assetRoot = resolveSpecPath(doc.at("asset_root").get<std::string>(), specDir);
        } else if (doc.contains("scene") || doc.contains("asset_root")) {
            return std::unexpected(
                "benchmark spec must set both 'scene' and 'asset_root' or neither");
        } else {
            cfg.scene = paths.content / "showroom" / "scene.json";
            cfg.assetRoot = paths.content / "showroom" / "assets";
        }

        const std::string binding = doc.value("binding", std::string("set"));
        if (binding == "set") {
            cfg.descriptorHeap = false;
        } else if (binding == "descriptor_heap" || binding == "heap") {
            cfg.descriptorHeap = true;
        } else {
            return std::unexpected("benchmark spec 'binding' must be 'set' or 'descriptor_heap'");
        }

        const std::string cull = doc.value("cull", std::string("gpu"));
        if (cull == "cpu") {
            cfg.cullMode = rhi::StaticCullMode::CpuFrustum;
        } else if (cull == "gpu") {
            cfg.cullMode = rhi::StaticCullMode::GpuCompute;
        } else {
            return std::unexpected("benchmark spec 'cull' must be 'cpu' or 'gpu'");
        }

        const std::string present = doc.value("present_mode", std::string("immediate"));
        if (present == "fifo") {
            cfg.presentMode = rhi::PresentMode::Fifo;
        } else if (present == "mailbox") {
            cfg.presentMode = rhi::PresentMode::Mailbox;
        } else if (present == "immediate") {
            cfg.presentMode = rhi::PresentMode::Immediate;
        } else {
            return std::unexpected(
                "benchmark spec 'present_mode' must be 'fifo', 'mailbox', or 'immediate'");
        }

        cfg.width = doc.value("width", 1280u);
        cfg.height = doc.value("height", 720u);
        if (cfg.width < 160u || cfg.width > 3840u || cfg.height < 120u || cfg.height > 2160u) {
            return std::unexpected("benchmark spec resolution is out of range");
        }
        cfg.shadows = doc.value("shadows", true);
        cfg.shadowResolution = doc.value("shadow_resolution", 2048u);
        if (cfg.shadows && (cfg.shadowResolution < 64u || cfg.shadowResolution > 4096u)) {
            return std::unexpected("benchmark spec 'shadow_resolution' must be in [64, 4096]");
        }
        cfg.exposureEV = doc.value("exposure_ev", 0.F);
        if (!std::isfinite(cfg.exposureEV) || std::abs(cfg.exposureEV) > 16.F) {
            return std::unexpected("benchmark spec 'exposure_ev' must be in [-16, 16]");
        }
        cfg.warmupFrames = doc.value("warmup_frames", 16u);
        cfg.sampleFrames = doc.value("sample_frames", 120u);
        cfg.repeats = doc.value("repeats", 1u);
        if (cfg.warmupFrames > 10000u || cfg.sampleFrames == 0u || cfg.sampleFrames > 100000u ||
            cfg.repeats == 0u || cfg.repeats > 20u) {
            return std::unexpected("benchmark spec frame counts or repeats are out of range");
        }
        const std::uint32_t cap = doc.value("capacity", 0u);
        cfg.capacity = cap == 0u ? UINT32_MAX : cap;
        cfg.hidden = doc.value("hidden", true);
        if (!doc.contains("output_dir") || !doc.at("output_dir").is_string() ||
            doc.at("output_dir").get<std::string>().empty()) {
            return std::unexpected("benchmark spec requires a non-empty 'output_dir'");
        }
        cfg.outputDir = resolveSpecPath(doc.at("output_dir").get<std::string>(), specDir);
    } catch (const Json::exception& e) {
        return std::unexpected(std::string("benchmark spec field error: ") + e.what());
    }
    return cfg;
}

render::StaticCamera evaluateBenchmarkCamera(
    const render::StaticCamera& baseCamera,
    std::uint32_t frameIndex,
    std::uint32_t totalFrames) noexcept {
    render::StaticCamera cam = baseCamera;
    const float t = totalFrames > 1u
                        ? static_cast<float>(frameIndex) / static_cast<float>(totalFrames - 1u)
                        : 0.F;
    const float phase = 2.F * std::numbers::pi_v<float> * t;
    // Sweep yaw +/-40 deg and slight pitch +/-4 deg so instances dynamically cross frustum planes.
    const float yawRad = 0.70F * std::sin(phase);
    const float pitchRad = 0.07F * std::cos(phase);
    const std::array<float, 4> qYaw{0.F, std::sin(yawRad * 0.5F), 0.F, std::cos(yawRad * 0.5F)};
    const std::array<float, 4> qPitch{
        std::sin(pitchRad * 0.5F), 0.F, 0.F, std::cos(pitchRad * 0.5F)};
    cam.orientation = normalizeQuat(mulQuat(qYaw, mulQuat(baseCamera.orientation, qPitch)));
    cam.position[0] = baseCamera.position[0] + 1.25F * std::sin(phase);
    return cam;
}

BenchmarkMetricStats
computeMetricStats(std::span<const double> validSamples, std::size_t missingCount) {
    BenchmarkMetricStats stats{};
    stats.missingCount = missingCount;
    std::vector<double> sorted;
    sorted.reserve(validSamples.size());
    for (double v : validSamples) {
        if (std::isfinite(v)) {
            sorted.push_back(v);
        } else {
            ++stats.missingCount;
        }
    }
    stats.sampleCount = sorted.size();
    if (sorted.empty()) {
        return stats;
    }
    std::ranges::sort(sorted);
    stats.minMs = sorted.front();
    stats.maxMs = sorted.back();
    stats.medianMs = linearQuantile(sorted, 0.50);
    stats.p95Ms = linearQuantile(sorted, 0.95);
    const double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    stats.meanMs = sum / static_cast<double>(sorted.size());
    return stats;
}

std::vector<BenchmarkFrameRecord> joinBenchmarkSamples(
    std::span<const BenchmarkCpuSample> cpuSamples,
    std::span<const rhi::GpuFrameTimings> gpuSamples,
    rhi::StaticCullMode cullMode) {
    std::unordered_map<std::uint64_t, const rhi::GpuFrameTimings*> gpuByFrame;
    gpuByFrame.reserve(gpuSamples.size());
    for (const auto& gpu : gpuSamples) {
        if (gpu.frameNumber != 0u) {
            gpuByFrame[gpu.frameNumber] = &gpu;
        }
    }

    std::vector<BenchmarkFrameRecord> joined;
    joined.reserve(cpuSamples.size());
    for (const auto& cpu : cpuSamples) {
        BenchmarkFrameRecord rec{};
        rec.repeatIndex = cpu.repeatIndex;
        rec.sampleIndex = cpu.sampleIndex;
        rec.frameNumber = cpu.timings.frameNumber;
        rec.cpuCullMs = cpu.timings.cullMs;
        rec.cpuRecordSubmitMs = cpu.timings.recordSubmitMs;
        rec.cpuTotalMs = cpu.timings.totalCpuMs;

        if (cullMode == rhi::StaticCullMode::CpuFrustum) {
            rec.visibleCount = cpu.timings.visibleCount;
            rec.drawCount = cpu.timings.drawCount;
            rec.overflowed = cpu.timings.overflowed;
        }

        const auto it = gpuByFrame.find(rec.frameNumber);
        if (it == gpuByFrame.end()) {
            rec.gpuAvailable = false;
            rec.gpuUnavailableReason = "missing completed GPU sample for frame";
        } else {
            const auto& gpu = *it->second;
            rec.gpuAvailable = gpu.available;
            rec.gpuUnavailableReason = gpu.unavailableReason;
            rec.gpuCullMs = gpu.cullMs;
            rec.gpuShadowMs = gpu.shadowMs;
            rec.gpuGeometryMs = gpu.geometryMs;
            rec.gpuLightingMs = gpu.lightingMs;
            rec.gpuDisplayMs = gpu.displayMs;
            rec.gpuTotalMs = gpu.totalGpuMs;
            if (cullMode == rhi::StaticCullMode::GpuCompute) {
                rec.visibleCount = gpu.visibleCount;
                rec.drawCount = gpu.drawCount;
                rec.overflowed = gpu.overflowed;
            }
        }
        joined.push_back(std::move(rec));
    }
    return joined;
}

std::expected<void, std::string> writeBenchmarkOutputs(
    const BenchmarkConfig& config,
    const BenchmarkSummaryMetadata& metadata,
    std::span<const BenchmarkFrameRecord> records) {
    // 1. Write frames.csv with full precision so summary stats are independently recomputable.
    std::ostringstream csv;
    csv << std::fixed << std::setprecision(6);
    csv << "repeat,sample_index,frame_number,visible_count,draw_count,overflowed,"
           "cpu_cull_ms,cpu_record_submit_ms,cpu_total_ms,"
           "gpu_available,gpu_cull_ms,gpu_shadow_ms,gpu_geometry_ms,gpu_lighting_ms,"
           "gpu_display_ms,gpu_total_ms,gpu_unavailable_reason\n";
    const auto optDouble = [&](const std::optional<double>& v) {
        if (v) {
            csv << *v;
        }
    };
    const auto optUint = [&](const std::optional<std::uint32_t>& v) {
        if (v) {
            csv << *v;
        }
    };
    for (const auto& r : records) {
        csv << r.repeatIndex << ',' << r.sampleIndex << ',' << r.frameNumber << ',';
        optUint(r.visibleCount);
        csv << ',';
        optUint(r.drawCount);
        csv << ',' << (r.overflowed ? 1 : 0) << ',';
        optDouble(r.cpuCullMs);
        csv << ',' << r.cpuRecordSubmitMs << ',' << r.cpuTotalMs << ',' << (r.gpuAvailable ? 1 : 0)
            << ',';
        optDouble(r.gpuCullMs);
        csv << ',';
        optDouble(r.gpuShadowMs);
        csv << ',';
        optDouble(r.gpuGeometryMs);
        csv << ',';
        optDouble(r.gpuLightingMs);
        csv << ',';
        optDouble(r.gpuDisplayMs);
        csv << ',';
        optDouble(r.gpuTotalMs);
        csv << ',';
        // Escape commas/quotes in reason if present.
        if (!r.gpuUnavailableReason.empty()) {
            csv << '"';
            for (char c : r.gpuUnavailableReason) {
                if (c == '"') {
                    csv << "\"\"";
                } else {
                    csv << c;
                }
            }
            csv << '"';
        }
        csv << '\n';
    }
    if (auto wrote = writeFileAtomic(config.outputDir / "frames.csv", csv.str()); !wrote) {
        return wrote;
    }

    // 2. Compute summary statistics strictly over non-overflow formal frames, separating truncated
    // samples explicitly.
    std::vector<double> cpuCull, cpuRecordSubmit, cpuTotal;
    std::vector<double> gpuCull, gpuShadow, gpuGeometry, gpuLighting, gpuDisplay, gpuTotal;
    std::vector<double> visSamples;
    std::size_t cpuCullMissing = 0u, gpuCullMissing = 0u, gpuShadowMissing = 0u;
    std::size_t gpuGeomMissing = 0u, gpuLightMissing = 0u, gpuDispMissing = 0u, gpuTotMissing = 0u;
    std::size_t overflowCount = 0u;
    std::size_t formalNonOverflowCount = 0u;
    std::set<std::string> gpuReasons;

    for (const auto& r : records) {
        if (r.overflowed) {
            ++overflowCount;
            continue;
        }
        ++formalNonOverflowCount;
        if (r.visibleCount) {
            visSamples.push_back(static_cast<double>(*r.visibleCount));
        }
        if (r.cpuCullMs) {
            cpuCull.push_back(*r.cpuCullMs);
        } else {
            ++cpuCullMissing;
        }
        cpuRecordSubmit.push_back(r.cpuRecordSubmitMs);
        cpuTotal.push_back(r.cpuTotalMs);

        if (!r.gpuUnavailableReason.empty()) {
            gpuReasons.insert(r.gpuUnavailableReason);
        }
        const auto collectGpu =
            [&](const std::optional<double>& val, std::vector<double>& dst, std::size_t& missing) {
                if (r.gpuAvailable && val) {
                    dst.push_back(*val);
                } else {
                    ++missing;
                }
            };
        collectGpu(r.gpuCullMs, gpuCull, gpuCullMissing);
        collectGpu(r.gpuShadowMs, gpuShadow, gpuShadowMissing);
        collectGpu(r.gpuGeometryMs, gpuGeometry, gpuGeomMissing);
        collectGpu(r.gpuLightingMs, gpuLighting, gpuLightMissing);
        collectGpu(r.gpuDisplayMs, gpuDisplay, gpuDispMissing);
        collectGpu(r.gpuTotalMs, gpuTotal, gpuTotMissing);
    }

    const auto visStats = computeMetricStats(visSamples, 0u);
    Json summary{
        {"version", kBenchmarkSpecVersion},
        {"name", config.name},
        {"preset_version", config.presetVersion},
        {"seed", config.seed},
        {"status", metadata.status},
        {"skip_reason", metadata.skipReason},
        {"scene", pathToUtf8(config.scene)},
        {"binding", config.descriptorHeap ? "descriptor_heap" : "set"},
        {"cull", std::string(cullModeName(config.cullMode))},
        {"draw_submission", "vkCmdDrawIndexedIndirectCount"},
        {"requested_present_mode", std::string(presentModeName(config.presentMode))},
        {"active_present_mode", std::string(presentModeName(metadata.activePresentMode))},
        {"resolution", Json::array({config.width, config.height})},
        {"shadows", config.shadows},
        {"shadow_resolution", config.shadows ? config.shadowResolution : 0u},
        {"exposure_ev", config.exposureEV},
        {"instance_count", metadata.instanceCount},
        {"capacity", config.capacity == UINT32_MAX ? 0u : config.capacity},
        {"warmup_frames", config.warmupFrames},
        {"sample_frames", config.sampleFrames},
        {"repeats", config.repeats},
        {"total_recorded_samples", records.size()},
        {"formal_non_overflow_samples", formalNonOverflowCount},
        {"overflow_sample_count", overflowCount},
        {"device_name", metadata.deviceName},
        {"device_api", metadata.deviceApi},
        {"build_configuration", metadata.buildConfiguration},
        {"build_identity", metadata.buildIdentity},
        {"visibility",
         {
             {"sample_count", visStats.sampleCount},
             {"min_visible", visStats.minMs ? Json(*visStats.minMs) : Json(nullptr)},
             {"median_visible", visStats.medianMs ? Json(*visStats.medianMs) : Json(nullptr)},
             {"max_visible", visStats.maxMs ? Json(*visStats.maxMs) : Json(nullptr)},
         }},
        {"metrics",
         {
             {"cpu_cull_ms", metricToJson(computeMetricStats(cpuCull, cpuCullMissing))},
             {"cpu_record_submit_ms", metricToJson(computeMetricStats(cpuRecordSubmit, 0u))},
             {"cpu_total_ms", metricToJson(computeMetricStats(cpuTotal, 0u))},
             {"gpu_cull_ms", metricToJson(computeMetricStats(gpuCull, gpuCullMissing), gpuReasons)},
             {"gpu_shadow_ms",
              metricToJson(computeMetricStats(gpuShadow, gpuShadowMissing), gpuReasons)},
             {"gpu_geometry_ms",
              metricToJson(computeMetricStats(gpuGeometry, gpuGeomMissing), gpuReasons)},
             {"gpu_lighting_ms",
              metricToJson(computeMetricStats(gpuLighting, gpuLightMissing), gpuReasons)},
             {"gpu_display_ms",
              metricToJson(computeMetricStats(gpuDisplay, gpuDispMissing), gpuReasons)},
             {"gpu_total_ms",
              metricToJson(computeMetricStats(gpuTotal, gpuTotMissing), gpuReasons)},
         }},
    };
    return writeFileAtomic(config.outputDir / "summary.json", summary.dump(2) + "\n");
}

int runBenchmark(const BenchmarkConfig& config, const AppPaths& paths) {
    const auto sceneFile = requireReadableFile(config.scene, "benchmark scene");
    if (!sceneFile) {
        std::fprintf(stderr, "%s\n", sceneFile.error().c_str());
        return 1;
    }
    const auto assetDir = requireReadableDirectory(config.assetRoot, "benchmark asset root");
    if (!assetDir) {
        std::fprintf(stderr, "%s\n", assetDir.error().c_str());
        return 1;
    }
    const auto shaderDir = requireReadableDirectory(paths.shaders, "shader directory");
    if (!shaderDir) {
        std::fprintf(stderr, "%s\n", shaderDir.error().c_str());
        return 1;
    }

    const core::BuildInfo& build = core::buildInfo();
    BenchmarkSummaryMetadata meta{};
    meta.buildConfiguration = build.configuration;
    meta.buildIdentity = build.buildId;

    // Probe device capability first so an unsupported Descriptor Heap run records an explicit
    // skipped_unsupported summary cleanly rather than pretending to have measured it.
    const fs::path diagRoot = config.outputDir / "diagnostics";
    {
        runtime::RuntimeConfig probe;
        probe.mode = runtime::QueryMode::Inspect;
        probe.query.requestedApi = rhi::makeApiVersion(1, 3);
        probe.diagnostics.outputRoot = diagRoot;
        probe.diagnostics.runId = "probe";
        auto probed = runtime::RuntimeSession::start(probe);
        if (!probed) {
            std::fputs("benchmark capability probe failed\n", stderr);
            return 1;
        }
        const auto& rep = (*probed)->report();
        meta.deviceName = rep.deviceName;
        meta.deviceApi = rep.deviceApi ? formatVulkanApi(*rep.deviceApi) : std::string("unknown");
        const bool heapSupported = rep.loaderApi >= rhi::kApiVersion1_4 &&
                                   rep.deviceApi >= rhi::kApiVersion1_4 &&
                                   rep.descriptorHeap == rhi::DescriptorHeapStatus::Supported;
        static_cast<void>((*probed)->close());
        if (config.descriptorHeap && !heapSupported) {
            meta.status = "skipped_unsupported";
            meta.skipReason = "device or loader does not support Vulkan 1.4 Descriptor Heap";
            if (auto wrote = writeBenchmarkOutputs(config, meta, {}); !wrote) {
                std::fprintf(stderr, "%s\n", wrote.error().c_str());
                return 1;
            }
            std::printf("benchmark=%s\nstatus=skipped_unsupported\n", config.name.c_str());
            return 0;
        }
    }

    auto gbufferVert = readShaderWords(*shaderDir / "gbuffer.vert.spv");
    auto gbufferFrag = readShaderWords(*shaderDir / "gbuffer_bindless.frag.spv");
    auto indirectVert = readShaderWords(*shaderDir / "static_indirect.vert.spv");
    auto indirectFrag = readShaderWords(*shaderDir / "static_indirect.frag.spv");
    auto fullscreenVert = readShaderWords(*shaderDir / "fullscreen.vert.spv");
    auto lightingFrag = readShaderWords(
        *shaderDir / (config.shadows ? "deferred_ibl_shadow.frag.spv" : "deferred_ibl.frag.spv"));
    auto displayFrag = readShaderWords(*shaderDir / "display_sdr.frag.spv");
    if (!gbufferVert || !gbufferFrag || !indirectVert || !indirectFrag || !fullscreenVert ||
        !lightingFrag || !displayFrag) {
        return 1;
    }

    std::vector<std::uint32_t> indirectComp;
    if (config.cullMode == rhi::StaticCullMode::GpuCompute) {
        auto comp = readShaderWords(*shaderDir / "static_indirect.comp.spv");
        if (!comp) {
            return 1;
        }
        indirectComp = std::move(*comp);
    }

    std::optional<rhi::ShadowMapConfig> shadowCfg;
    if (config.shadows) {
        auto shadowDirect = readShaderWords(*shaderDir / "shadow_direct.vert.spv");
        auto shadowIndirect = readShaderWords(*shaderDir / "shadow_indirect.vert.spv");
        if (!shadowDirect || !shadowIndirect) {
            return 1;
        }
        shadowCfg.emplace(
            rhi::ShadowMapConfig{
                std::move(*shadowDirect), std::move(*shadowIndirect), config.shadowResolution});
    }

    runtime::RuntimeConfig rt;
    rt.mode = config.descriptorHeap ? runtime::QueryMode::RequireProduct
                                    : runtime::QueryMode::Development;
    rt.query.requestedApi = config.descriptorHeap ? rhi::kApiVersion1_4 : rhi::makeApiVersion(1, 3);
    rt.query.createGraphicsDevice = true;
    rt.query.enableDescriptorHeap = config.descriptorHeap;
    rt.query.enableSetBinding = !config.descriptorHeap;
    rt.query.enableGBuffer = true;
    rt.query.enableScreenshot = false; // No readbacks or screenshots during timing round!
    rt.query.triangleShaders =
        rhi::TriangleShaders{std::move(*gbufferVert), std::move(*gbufferFrag)};
    rt.query.staticIndirect = rhi::StaticIndirectShaders{
        {std::move(*indirectVert), std::move(*indirectFrag)},
        std::move(indirectComp),
        config.capacity,
        config.cullMode,
        nullptr,
        true};
    rt.query.deferredLighting = rhi::DeferredLighting{
        std::move(*fullscreenVert),
        std::move(*lightingFrag),
        std::move(*displayFrag),
        config.exposureEV,
        true,
        std::move(shadowCfg)};
    rt.staticAssetRoot = *assetDir;
    rt.presentMode = config.presentMode;
    rt.window.emplace();
    rt.window->title = "AR Engine Benchmark";
    rt.window->width = static_cast<int>(config.width);
    rt.window->height = static_cast<int>(config.height);
    rt.window->visible = !config.hidden;
    rt.diagnostics.outputRoot = diagRoot;
    rt.diagnostics.runId = "bench-run";

    auto session = runtime::RuntimeSession::start(rt);
    if (!session) {
        std::fputs("benchmark session start failed\n", stderr);
        return 1;
    }
    if (auto loaded = (*session)->openStaticScene(*sceneFile); !loaded) {
        std::fprintf(
            stderr, "benchmark openStaticScene failed: %s\n", loaded.error().message.c_str());
        static_cast<void>((*session)->close());
        return 1;
    }
    {
        const auto packed = render::packEnvironmentLighting(
            render::bakeEnvironmentLighting(render::EnvironmentPreset::Directional));
        if (auto envOk = (*session)->replaceEnvironment(
                {packed.diffuse, packed.specular, packed.brdf}, false);
            !envOk) {
            std::fprintf(
                stderr, "benchmark replaceEnvironment failed: %s\n", envOk.error().message.c_str());
            static_cast<void>((*session)->close());
            return 1;
        }
    }

    const auto baseCamera = (*session)->activeCamera();
    if (!baseCamera) {
        std::fputs("benchmark scene has no active camera\n", stderr);
        static_cast<void>((*session)->close());
        return 1;
    }
    meta.deviceName = (*session)->report().deviceName;
    meta.deviceApi = (*session)->report().deviceApi
                         ? formatVulkanApi(*(*session)->report().deviceApi)
                         : std::string("unknown");
    meta.instanceCount = (*session)->activeInstanceCount();
    meta.activePresentMode = (*session)->activePresentMode();

    std::vector<BenchmarkCpuSample> cpuSamples;
    std::vector<rhi::GpuFrameTimings> gpuSamples;
    cpuSamples.reserve(static_cast<std::size_t>(config.repeats) * config.sampleFrames);
    gpuSamples.reserve(static_cast<std::size_t>(config.repeats) * config.sampleFrames);

    const auto presentOne = [&](const render::StaticCamera& cam,
                                bool recordSample,
                                std::uint32_t repeatIndex,
                                std::uint32_t sampleIndex) -> bool {
        if (auto ok = (*session)->setCamera(cam); !ok) {
            std::fprintf(stderr, "benchmark setCamera failed: %s\n", ok.error().message.c_str());
            return false;
        }
        while (true) {
            const auto status = (*session)->presentFrame(nullptr, nullptr, true);
            if (!status) {
                std::fprintf(
                    stderr, "benchmark presentFrame failed: %s\n", status.error().message.c_str());
                return false;
            }
            if (*status == runtime::FrameStatus::Closed) {
                std::fputs("benchmark window closed unexpectedly\n", stderr);
                return false;
            }
            if (*status == runtime::FrameStatus::Presented) {
                if (recordSample) {
                    cpuSamples.push_back(
                        BenchmarkCpuSample{repeatIndex, sampleIndex, (*session)->lastCpuTimings()});
                }
                auto completed = (*session)->takeCompletedGpuTimings();
                if (recordSample) {
                    gpuSamples.insert(gpuSamples.end(), completed.begin(), completed.end());
                }
                return true;
            }
        }
    };

    for (std::uint32_t repeat = 0u; repeat < config.repeats; ++repeat) {
        for (std::uint32_t w = 0u; w < config.warmupFrames; ++w) {
            const auto cam =
                evaluateBenchmarkCamera(*baseCamera, w % config.sampleFrames, config.sampleFrames);
            if (!presentOne(cam, false, repeat, 0u)) {
                static_cast<void>((*session)->close());
                return 1;
            }
        }
        // Drain warmup frames so warmup GPU timings never leak into the formal measurement round.
        if (auto drainedWarmup = (*session)->drainCompletedGpuTimings(); !drainedWarmup) {
            std::fprintf(
                stderr,
                "benchmark warmup drain failed: %s\n",
                drainedWarmup.error().message.c_str());
            static_cast<void>((*session)->close());
            return 1;
        }

        for (std::uint32_t s = 0u; s < config.sampleFrames; ++s) {
            const auto cam = evaluateBenchmarkCamera(*baseCamera, s, config.sampleFrames);
            if (!presentOne(cam, true, repeat, s)) {
                static_cast<void>((*session)->close());
                return 1;
            }
        }
        auto tailGpu = (*session)->drainCompletedGpuTimings();
        if (!tailGpu) {
            std::fprintf(
                stderr, "benchmark tail drain failed: %s\n", tailGpu.error().message.c_str());
            static_cast<void>((*session)->close());
            return 1;
        }
        gpuSamples.insert(gpuSamples.end(), tailGpu->begin(), tailGpu->end());
    }

    const core::EndResult closed = (*session)->close();
    if (closed.result != core::ResultKind::Success) {
        std::fputs("benchmark session did not close cleanly\n", stderr);
        return 1;
    }

    const auto joined = joinBenchmarkSamples(cpuSamples, gpuSamples, config.cullMode);
    if (auto wrote = writeBenchmarkOutputs(config, meta, joined); !wrote) {
        std::fprintf(stderr, "%s\n", wrote.error().c_str());
        return 1;
    }
    std::printf(
        "benchmark=%s\nstatus=completed\ninstances=%u\nsamples=%zu\npresent_mode=%.*s\n",
        config.name.c_str(),
        meta.instanceCount,
        joined.size(),
        static_cast<int>(presentModeName(meta.activePresentMode).size()),
        presentModeName(meta.activePresentMode).data());
    std::fflush(stdout);
    return 0;
}

} // namespace ar::app
