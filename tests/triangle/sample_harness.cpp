// Test-only multi-mode sample driver for legacy P1-P5 CTest regressions.
// The primary product executable is `ar_engine` (examples/triangle/main.cpp).

#include "render/CullReference.h"
#include "render/DirectionalShadow.h"
#include "render/EnvironmentLighting.h"
#include "render/FreeCameraController.h"
#include "runtime/RuntimeSession.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace {

namespace fs = std::filesystem;

struct HarnessOptions {
    std::optional<std::uint32_t> frames;
    bool visible = true;
    bool showroom = false;
    bool clearOnly = false;
    bool gpuDriven = false;
    bool gpuEmpty = false;
    bool setBinding = false;
    bool descriptorHeap = false;
    bool verifyBindingRetirement = false;
    bool gbuffer = false;
    bool bindlessMaterials = false;
    bool staticGpuDriven = false;
    std::optional<std::uint32_t> indirectCapacity;
    bool hdrLighting = false;
    bool shadows = false;
    std::optional<std::uint32_t> shadowResolution;
    bool freeCamera = false;
    bool verifyIblRollback = false;
    std::optional<ar::render::EnvironmentPreset> ibl;
    std::optional<float> exposureEV;
    std::optional<fs::path> gbufferProbes;
    std::optional<fs::path> scene;
    std::optional<fs::path> assetRoot;
    std::optional<fs::path> sceneCandidate;
    std::optional<fs::path> screenshot;
    fs::path output;
};

std::string pathToUtf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::optional<fs::path> requireReadableFile(const fs::path& path, std::string_view role) {
    std::error_code error;
    const fs::path expanded = fs::weakly_canonical(path, error);
    const fs::path& display = error ? path : expanded;
    error.clear();
    if (path.empty() || !fs::is_regular_file(path, error) || error) {
        std::fprintf(
            stderr,
            "missing %.*s: %s\n",
            static_cast<int>(role.size()),
            role.data(),
            pathToUtf8(display).c_str());
        return std::nullopt;
    }
    return display;
}

std::optional<fs::path> requireReadableDirectory(const fs::path& path, std::string_view role) {
    std::error_code error;
    const fs::path expanded = fs::weakly_canonical(path, error);
    const fs::path& display = error ? path : expanded;
    error.clear();
    if (path.empty() || !fs::is_directory(path, error) || error) {
        std::fprintf(
            stderr,
            "missing %.*s: %s\n",
            static_cast<int>(role.size()),
            role.data(),
            pathToUtf8(display).c_str());
        return std::nullopt;
    }
    return display;
}

std::optional<HarnessOptions> parseHarnessOptions(
    std::span<const std::wstring_view> args,
    const fs::path& contentDir,
    const fs::path& defaultOutput) {
    HarnessOptions options;
    options.output = defaultOutput;
    bool explicitOutput = false;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::wstring_view arg = args[index];
        if (arg == L"--hidden") {
            options.visible = false;
        } else if (arg == L"--showroom") {
            options.showroom = true;
        } else if (arg == L"--set-binding-retirement") {
            options.setBinding = true;
            options.verifyBindingRetirement = true;
        } else if (arg == L"--gpu-driven" || arg == L"--gpu-driven-empty") {
            options.gpuDriven = true;
            options.gpuEmpty = arg == L"--gpu-driven-empty";
        } else if (arg == L"--set-binding") {
            options.setBinding = true;
        } else if (arg == L"--descriptor-heap") {
            options.descriptorHeap = true;
            options.gbuffer = true;
        } else if (arg == L"--clear-only") {
            options.clearOnly = true;
        } else if (arg == L"--ibl" && !options.ibl && index + 1 < args.size()) {
            const std::wstring_view preset = args[++index];
            if (preset == L"constant") {
                options.ibl = ar::render::EnvironmentPreset::Constant;
            } else if (preset == L"directional") {
                options.ibl = ar::render::EnvironmentPreset::Directional;
            } else {
                return std::nullopt;
            }
            options.hdrLighting = true;
            options.gbuffer = true;
        } else if (arg == L"--verify-ibl-rollback") {
            options.verifyIblRollback = true;
        } else if (arg == L"--hdr-lighting") {
            options.hdrLighting = true;
            options.gbuffer = true;
        } else if (arg == L"--shadows") {
            options.shadows = true;
        } else if (arg == L"--free-camera") {
            options.freeCamera = true;
        } else if (
            arg == L"--shadow-resolution" && !options.shadowResolution && index + 1 < args.size()) {
            const std::wstring_view value = args[++index];
            if (value.empty() || value.find_first_not_of(L"0123456789") != value.npos) {
                return std::nullopt;
            }
            const std::wstring text(value);
            errno = 0;
            const unsigned long long parsed = std::wcstoull(text.c_str(), nullptr, 10);
            if (errno == ERANGE || parsed < ar::render::kMinShadowResolution ||
                parsed > ar::render::kMaxShadowResolution) {
                return std::nullopt;
            }
            options.shadowResolution = static_cast<std::uint32_t>(parsed);
        } else if (arg == L"--exposure-ev" && !options.exposureEV && index + 1 < args.size()) {
            const std::wstring text(args[++index]);
            wchar_t* end = nullptr;
            errno = 0;
            const float exposure = std::wcstof(text.c_str(), &end);
            if (end == text.c_str() || *end != L'\0' || errno == ERANGE ||
                !std::isfinite(exposure) || std::abs(exposure) > 16.F) {
                return std::nullopt;
            }
            options.exposureEV = exposure;
        } else if (arg == L"--static-gpu-driven") {
            options.staticGpuDriven = true;
            options.bindlessMaterials = true;
            options.gbuffer = true;
        } else if (arg == L"--bindless-materials") {
            options.bindlessMaterials = true;
            options.gbuffer = true;
        } else if (arg == L"--gbuffer") {
            options.gbuffer = true;
        } else if (
            arg == L"--gbuffer-probes" && !options.gbufferProbes && index + 1 < args.size()) {
            options.gbuffer = true;
            options.gbufferProbes = fs::path(args[++index]);
        } else if (arg == L"--static-scene" && !options.scene && index + 1 < args.size()) {
            options.scene = fs::path(args[++index]);
        } else if (arg == L"--asset-root" && !options.assetRoot && index + 1 < args.size()) {
            options.assetRoot = fs::path(args[++index]);
        } else if (
            arg == L"--static-scene-test" && !options.sceneCandidate && index + 1 < args.size()) {
            options.sceneCandidate = fs::path(args[++index]);
        } else if (
            (arg == L"--frames" || arg == L"--indirect-capacity") && index + 1 < args.size()) {
            auto& target = arg == L"--frames" ? options.frames : options.indirectCapacity;
            if (target) {
                return std::nullopt;
            }
            const std::wstring_view value = args[++index];
            if (value.empty() || value.find_first_not_of(L"0123456789") != value.npos) {
                return std::nullopt;
            }
            const std::wstring text(value);
            errno = 0;
            const unsigned long long parsed = std::wcstoull(text.c_str(), nullptr, 10);
            if (errno == ERANGE || parsed > std::numeric_limits<std::uint32_t>::max() ||
                (arg == L"--frames" && parsed == 0)) {
                return std::nullopt;
            }
            target = static_cast<std::uint32_t>(parsed);
        } else if (arg == L"--screenshot" && !options.screenshot && index + 1 < args.size()) {
            options.screenshot = fs::path(args[++index]);
            if (options.screenshot->empty()) {
                return std::nullopt;
            }
        } else if (arg == L"--output" && index + 1 < args.size()) {
            options.output = fs::path(args[++index]);
            if (options.output.empty()) {
                return std::nullopt;
            }
            explicitOutput = true;
        } else {
            return std::nullopt;
        }
    }

    if (options.showroom) {
        if (options.scene || options.assetRoot || options.sceneCandidate || options.clearOnly ||
            options.setBinding || options.verifyBindingRetirement || options.gpuDriven ||
            contentDir.empty()) {
            return std::nullopt;
        }
        options.scene = contentDir / "showroom" / "scene.json";
        options.assetRoot = contentDir / "showroom" / "assets";
        options.gbuffer = true;
        options.bindlessMaterials = true;
        options.staticGpuDriven = true;
        options.hdrLighting = true;
        if (!options.ibl) {
            options.ibl = ar::render::EnvironmentPreset::Directional;
        }
    }

    if ((!explicitOutput && options.output.empty()) ||
        ((!options.visible || options.screenshot || options.gbufferProbes) && !options.frames)) {
        return std::nullopt;
    }
    if (options.verifyBindingRetirement && (options.frames != 3 || options.screenshot)) {
        return std::nullopt;
    }
    if (options.clearOnly && options.setBinding) {
        return std::nullopt;
    }
    if (options.gpuDriven && (options.clearOnly || options.setBinding)) {
        return std::nullopt;
    }
    if (options.descriptorHeap && (options.setBinding || options.clearOnly || options.gpuDriven)) {
        return std::nullopt;
    }
    if ((options.indirectCapacity && !options.staticGpuDriven) ||
        (options.verifyIblRollback && (!options.ibl || !options.frames || *options.frames < 2)) ||
        (options.exposureEV && !options.hdrLighting) ||
        (options.shadowResolution && !options.shadows) ||
        (options.shadows && (!options.hdrLighting || !options.scene)) ||
        (options.freeCamera && (!options.visible || !options.scene)) ||
        (options.gbuffer && !options.scene) ||
        (options.gbufferProbes && options.gbufferProbes->empty()) ||
        options.scene.has_value() != options.assetRoot.has_value() ||
        (options.scene && (options.scene->empty() || options.assetRoot->empty() ||
                           options.gpuDriven || options.clearOnly || options.setBinding)) ||
        (options.sceneCandidate &&
         (!options.scene || !options.screenshot || options.frames != 4))) {
        return std::nullopt;
    }
    return options;
}

bool writeGBufferProbes(const fs::path& path, const ar::rhi::FrameScreenshot& frame) {
    constexpr std::array names{"overlap", "far-only", "background"};
    if (frame.gbuffer.size() != names.size()) {
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    out.precision(9);
    const auto array = [&](const auto& values) {
        out << '[';
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) {
                out << ',';
            }
            out << values[i];
        }
        out << ']';
    };
    out << "{\"extent\":";
    array(std::array{frame.width, frame.height});
    if (frame.indirect) {
        out << ",\"culling\":{\"visible\":" << frame.indirect->visible << ",\"drawn_ids\":";
        array(frame.indirect->drawnIds);
        out << '}';
    }
    out << ",\"probes\":[";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) {
            out << ',';
        }
        const auto& probe = frame.gbuffer[i];
        out << "{\"name\":\"" << names[i] << "\",\"pixel\":";
        array(probe.pixel);
        out << ",\"depth\":" << probe.depth << ",\"base_color\":";
        array(probe.baseColor);
        out << ",\"view_normal\":";
        array(probe.normal);
        out << ",\"metallic_roughness\":";
        array(probe.material);
        out << ",\"view_position\":";
        array(probe.viewPosition);
        if (probe.hdrColor) {
            out << ",\"hdr_color\":";
            array(*probe.hdrColor);
            out << ",\"sdr_rgb\":";
            const auto offset =
                (static_cast<std::size_t>(probe.pixel[1]) * frame.width + probe.pixel[0]) * 4;
            array(
                std::array{
                    static_cast<unsigned>(frame.rgba[offset]),
                    static_cast<unsigned>(frame.rgba[offset + 1]),
                    static_cast<unsigned>(frame.rgba[offset + 2])});
        }
        out << '}';
    }
    out << "]}\n";
    out.close();
    return out.good();
}

bool writeScreenshot(const fs::path& path, const ar::rhi::FrameScreenshot& image) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    const int written = stbi_write_png_to_func(
        [](void* context, void* bytes, int size) {
            static_cast<std::ofstream*>(context)->write(static_cast<const char*>(bytes), size);
        },
        &stream,
        static_cast<int>(image.width),
        static_cast<int>(image.height),
        4,
        image.rgba.data(),
        static_cast<int>(image.width * 4));
    stream.close();
    return written != 0 && static_cast<bool>(stream);
}

std::optional<std::vector<std::uint32_t>> readShader(const fs::path& path) {
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

void printError(const ar::core::Error& error) {
    std::fprintf(stderr, "%s: %s", ar::core::toString(error.category), error.message.c_str());
    if (error.origin) {
        std::fprintf(
            stderr,
            " (%s %lld)",
            error.origin->domain.c_str(),
            static_cast<long long>(error.origin->code));
    }
    std::fputc('\n', stderr);
}

void reportEndErrors(const ar::core::EndResult& result) {
    if (result.primaryError) {
        printError(*result.primaryError);
    }
    for (const auto& error : result.cleanupErrors) {
        printError(error);
    }
    for (const auto& error : result.diagnosticErrors) {
        printError(error);
    }
}

void closeAndReport(ar::runtime::RuntimeSession& session) {
    reportEndErrors(session.close());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const fs::path shaderRoot(AR_SAMPLE_HARNESS_SHADER_DIR);
    const fs::path contentRoot(AR_SAMPLE_HARNESS_CONTENT_DIR);
    const fs::path defaultOutput(AR_SAMPLE_HARNESS_OUTPUT_ROOT);

    std::vector<std::wstring_view> args;
    if (argc > 1) {
        args.reserve(static_cast<std::size_t>(argc - 1));
        for (int index = 1; index < argc; ++index) {
            args.emplace_back(argv[index]);
        }
    }
    const auto options = parseHarnessOptions(args, contentRoot, defaultOutput);
    if (!options) {
        std::fputs("invalid ar_sample_harness arguments\n", stderr);
        return 2;
    }

    std::optional<fs::path> sceneFile;
    std::optional<fs::path> assetDir;
    if (options->scene) {
        sceneFile = requireReadableFile(*options->scene, "scene file");
        if (!sceneFile) {
            return 1;
        }
        assetDir = requireReadableDirectory(*options->assetRoot, "asset root");
        if (!assetDir) {
            return 1;
        }
    }

    ar::runtime::RuntimeConfig config;
    config.mode = options->descriptorHeap ? ar::runtime::QueryMode::RequireProduct
                                          : ar::runtime::QueryMode::Development;
    config.query.requestedApi =
        options->descriptorHeap ? ar::rhi::kApiVersion1_4 : ar::rhi::makeApiVersion(1, 3);
    config.query.createGraphicsDevice = true;
    config.query.enableScreenshot = options->screenshot.has_value() ||
                                    options->gbufferProbes.has_value() ||
                                    options->verifyBindingRetirement;
    config.query.enableDescriptorHeap = options->descriptorHeap;
    config.query.enableSetBinding =
        !options->descriptorHeap && (options->setBinding || options->bindlessMaterials);
    config.query.enableSetBindingSample = options->setBinding;
    config.query.verifyBindingRetirement = options->verifyBindingRetirement;
    config.query.enableGBuffer = options->gbuffer;
    config.staticAssetRoot = assetDir;
    if (options->verifyBindingRetirement) {
        config.diagnostics.minLevel = ar::core::LogLevel::Debug;
    }
    if (!options->clearOnly) {
        const auto checkedShaderDir = requireReadableDirectory(shaderRoot, "shader directory");
        if (!checkedShaderDir) {
            return 1;
        }
        const fs::path& shaderDir = *checkedShaderDir;
        auto vertex = readShader(
            shaderDir / (options->gbuffer      ? "gbuffer.vert.spv"
                         : options->scene      ? "static_scene.vert.spv"
                         : options->gpuDriven  ? "gpu_draw.vert.spv"
                         : options->setBinding ? "set_binding.vert.spv"
                                               : "triangle.vert.spv"));
        auto fragment = readShader(
            shaderDir / ((options->bindlessMaterials || options->descriptorHeap)
                             ? "gbuffer_bindless.frag.spv"
                         : options->gbuffer    ? "gbuffer.frag.spv"
                         : options->scene      ? "static_scene.frag.spv"
                         : options->gpuDriven  ? "gpu_draw.frag.spv"
                         : options->setBinding ? "set_binding.frag.spv"
                                               : "triangle.frag.spv"));
        if (options->hdrLighting) {
            auto fullscreen = readShader(shaderDir / "fullscreen.vert.spv");
            const char* directShaderName =
                options->ibl
                    ? (options->shadows ? "deferred_ibl_shadow.frag.spv" : "deferred_ibl.frag.spv")
                    : (options->shadows ? "deferred_shadow.frag.spv" : "deferred_direct.frag.spv");
            auto direct = readShader(shaderDir / directShaderName);
            auto display = readShader(shaderDir / "display_sdr.frag.spv");
            if (!fullscreen || !direct || !display) {
                return 1;
            }
            std::optional<ar::rhi::ShadowMapConfig> shadowConfig;
            if (options->shadows) {
                auto shadowDirect = readShader(shaderDir / "shadow_direct.vert.spv");
                if (!shadowDirect) {
                    return 1;
                }
                std::vector<std::uint32_t> shadowIndirectWords;
                if (options->staticGpuDriven) {
                    auto shadowIndirect = readShader(shaderDir / "shadow_indirect.vert.spv");
                    if (!shadowIndirect) {
                        return 1;
                    }
                    shadowIndirectWords = std::move(*shadowIndirect);
                }
                shadowConfig.emplace(
                    ar::rhi::ShadowMapConfig{
                        std::move(*shadowDirect),
                        std::move(shadowIndirectWords),
                        options->shadowResolution.value_or(2048u)});
            }
            config.query.deferredLighting.emplace(
                ar::rhi::DeferredLighting{
                    std::move(*fullscreen),
                    std::move(*direct),
                    std::move(*display),
                    options->exposureEV.value_or(0.F),
                    options->ibl.has_value(),
                    std::move(shadowConfig)});
        }
        if (options->staticGpuDriven) {
            auto indirectVertex = readShader(shaderDir / "static_indirect.vert.spv");
            auto indirectFragment = readShader(shaderDir / "static_indirect.frag.spv");
            auto compute = readShader(shaderDir / "static_indirect.comp.spv");
            if (!indirectVertex || !indirectFragment || !compute) {
                return 1;
            }
            config.query.staticIndirect.emplace(
                ar::rhi::StaticIndirectShaders{
                    {std::move(*indirectVertex), std::move(*indirectFragment)},
                    std::move(*compute),
                    options->indirectCapacity.value_or(UINT32_MAX)});
        }
        if (options->gpuDriven) {
            auto compute = readShader(shaderDir / "gpu_cull.comp.spv");
            if (!compute) {
                return 1;
            }
            auto scene = ar::render::fixedCullScene();
            auto objects = std::span<const ar::render::CullObject>{scene.objects};
            if (options->gpuEmpty) {
                objects = objects.subspan(1, 1);
            }
            const auto planes = std::as_bytes(std::span{scene.frustum});
            const auto records = std::as_bytes(objects);
            std::vector<float> positions;
            positions.reserve(scene.vertices.size() * 3);
            for (const auto& position : scene.vertices) {
                positions.insert(positions.end(), position.begin(), position.end());
            }
            config.query.gpuDriven = ar::rhi::GpuDrivenInput{
                {planes.begin(), planes.end()},
                {records.begin(), records.end()},
                std::move(*compute),
                std::move(positions)};
        }
        if (!vertex || !fragment) {
            return 1;
        }
        config.query.triangleShaders =
            ar::rhi::TriangleShaders{std::move(*vertex), std::move(*fragment)};
    }
    config.window.emplace();
    config.window->title = "AR Engine Sample Harness";
    config.window->visible = options->visible;
    config.diagnostics.outputRoot = options->output;
    const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    config.diagnostics.runId = "harness-" + std::to_string(timestamp);

    auto session = ar::runtime::RuntimeSession::start(config);
    if (!session) {
        reportEndErrors(session.error().endResult);
        return 1;
    }
    if (sceneFile) {
        if (auto loaded = (*session)->openStaticScene(*sceneFile); !loaded) {
            printError(loaded.error());
            closeAndReport(**session);
            return 1;
        }
    }

    const auto replaceEnvironment = [&](ar::render::EnvironmentPreset preset, bool verifyRollback) {
        const auto packed =
            ar::render::packEnvironmentLighting(ar::render::bakeEnvironmentLighting(preset));
        return (*session)->replaceEnvironment(
            {packed.diffuse, packed.specular, packed.brdf}, verifyRollback);
    };
    if (options->ibl) {
        if (auto loaded = replaceEnvironment(*options->ibl, false); !loaded) {
            printError(loaded.error());
            closeAndReport(**session);
            return 1;
        }
    }

    unsigned long long presented = 0;
    ar::rhi::FrameScreenshot screenshot;
    while (!options->frames || presented < *options->frames) {
        const bool capture =
            options->sceneCandidate ||
            ((options->screenshot || options->gbufferProbes) && presented + 1 == *options->frames);
        const auto frame = (*session)->presentFrame(capture ? &screenshot : nullptr);
        if (!frame || *frame == ar::runtime::FrameStatus::Closed) {
            break;
        }
        if (*frame == ar::runtime::FrameStatus::Presented) {
            ++presented;
            if (options->verifyIblRollback && presented == 1) {
                const auto replacement = *options->ibl == ar::render::EnvironmentPreset::Constant
                                             ? ar::render::EnvironmentPreset::Directional
                                             : ar::render::EnvironmentPreset::Constant;
                auto rejected = replaceEnvironment(replacement, true);
                if (rejected || rejected.error().message !=
                                    "environment diffuse: injected failure after diffuse upload") {
                    std::fputs(
                        "expected failure after candidate environment diffuse upload\n", stderr);
                    closeAndReport(**session);
                    return 1;
                }
            }
            if (options->sceneCandidate && presented < 4) {
                constexpr std::array names{"before.png", "rollback.png", "reopened.png"};
                const auto directory = options->screenshot->parent_path();
                if (!writeScreenshot(directory / names[presented - 1], screenshot)) {
                    closeAndReport(**session);
                    return 1;
                }
                std::expected<void, ar::core::Error> change;
                if (presented == 1) {
                    auto rejected = (*session)->openStaticScene(*options->sceneCandidate, true);
                    if (rejected || rejected.error().message !=
                                        "injected failure after first candidate mesh upload") {
                        std::fputs("expected failure after first candidate mesh upload\n", stderr);
                        closeAndReport(**session);
                        return 1;
                    }
                } else if (presented == 2) {
                    const auto saved = directory / "saved.json";
                    change = (*session)->saveStaticScene(saved);
                    if (change) {
                        change = (*session)->openStaticScene(saved);
                    }
                } else {
                    change = (*session)->openStaticScene(*options->sceneCandidate);
                }
                if (!change) {
                    printError(change.error());
                    closeAndReport(**session);
                    return 1;
                }
            }
        }
    }
    const ar::core::EndResult result = (*session)->close();
    std::printf("presented_frames=%llu\n", presented);
    reportEndErrors(result);
    if (result.result != ar::core::ResultKind::Success) {
        return 1;
    }
    if (options->frames && presented != *options->frames) {
        std::fputs("window closed before the requested frame count\n", stderr);
        return 1;
    }
    if (options->screenshot && !writeScreenshot(*options->screenshot, screenshot)) {
        std::fwprintf(stderr, L"Could not write screenshot: %ls\n", options->screenshot->c_str());
        return 1;
    }
    if (options->gbufferProbes && !writeGBufferProbes(*options->gbufferProbes, screenshot)) {
        std::fputs("could not write GBuffer probes\n", stderr);
        return 1;
    }
    return 0;
}
