#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/RuntimeSession.h"
#include "render/CullReference.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace {

struct Options {
    std::optional<std::uint32_t> frames;
    bool visible = true;
    bool clearOnly = false;
    bool gpuDriven = false;
    bool gpuEmpty = false;
    bool setBinding = false;
    bool verifyBindingRetirement = false;
    bool gbuffer = false;
    bool hdrLighting = false;
    std::optional<float> exposureEV;
    std::optional<std::filesystem::path> gbufferProbes;
    std::optional<std::filesystem::path> scene;
    std::optional<std::filesystem::path> assetRoot;
    std::optional<std::filesystem::path> sceneCandidate;
    std::optional<std::filesystem::path> screenshot;
    std::filesystem::path output = AR_TRIANGLE_OUTPUT_ROOT;
};

std::optional<Options> parse(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view arg = argv[index];
        if (arg == L"--hidden") {
            options.visible = false;
        } else if (arg == L"--set-binding-retirement") {
            options.setBinding = true;
            options.verifyBindingRetirement = true;
        } else if (arg == L"--gpu-driven" || arg == L"--gpu-driven-empty") {
            options.gpuDriven = true;
            options.gpuEmpty = arg == L"--gpu-driven-empty";
        } else if (arg == L"--set-binding") {
            options.setBinding = true;
        } else if (arg == L"--clear-only") {
            options.clearOnly = true;
        } else if (arg == L"--hdr-lighting") {
            options.hdrLighting = true;
            options.gbuffer = true;
        } else if (arg == L"--exposure-ev" && !options.exposureEV && index + 1 < argc) {
            const wchar_t* text = argv[++index];
            wchar_t* end = nullptr;
            errno = 0;
            const float exposure = std::wcstof(text, &end);
            if (end == text || *end != L'\0' || errno == ERANGE ||
                !std::isfinite(exposure) || std::abs(exposure) > 16.F) {
                return std::nullopt;
            }
            options.exposureEV = exposure;
        } else if (arg == L"--gbuffer") {
            options.gbuffer = true;
        } else if (arg == L"--gbuffer-probes" && !options.gbufferProbes && index + 1 < argc) {
            options.gbuffer = true;
            options.gbufferProbes = argv[++index];
        } else if (arg == L"--static-scene" && !options.scene && index + 1 < argc) {
            options.scene = argv[++index];
        } else if (arg == L"--asset-root" && !options.assetRoot && index + 1 < argc) {
            options.assetRoot = argv[++index];
        } else if (arg == L"--static-scene-test" && !options.sceneCandidate && index + 1 < argc) {
            options.sceneCandidate = argv[++index];
        } else if (arg == L"--frames" && !options.frames && index + 1 < argc) {
            const std::wstring_view value = argv[++index];
            if (value.empty() || value.find_first_not_of(L"0123456789") != value.npos) {
                return std::nullopt;
            }
            errno = 0;
            const unsigned long frames = std::wcstoul(value.data(), nullptr, 10);
            if (errno == ERANGE || frames == 0) {
                return std::nullopt;
            }
            options.frames = static_cast<std::uint32_t>(frames);
        } else if (arg == L"--screenshot" && !options.screenshot && index + 1 < argc) {
            options.screenshot = argv[++index];
            if (options.screenshot->empty()) {
                return std::nullopt;
            }
        } else if (arg == L"--output" && index + 1 < argc) {
            options.output = argv[++index];
            if (options.output.empty()) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }
    if ((!options.visible || options.screenshot || options.gbufferProbes) && !options.frames) {
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
    if ((options.exposureEV && !options.hdrLighting) || (options.gbuffer && !options.scene) ||
        (options.gbufferProbes && options.gbufferProbes->empty()) ||
        options.scene.has_value() != options.assetRoot.has_value() ||
        (options.scene && (options.scene->empty() || options.assetRoot->empty() ||
                           options.gpuDriven || options.clearOnly || options.setBinding)) ||
        (options.sceneCandidate && (!options.scene || !options.screenshot || options.frames != 4))) {
        return std::nullopt;
    }
    return options;
}

bool writeGBufferProbes(const std::filesystem::path& path, const ar::rhi::FrameScreenshot& frame) {
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
            array(std::array{static_cast<unsigned>(frame.rgba[offset]),
                             static_cast<unsigned>(frame.rgba[offset + 1]),
                             static_cast<unsigned>(frame.rgba[offset + 2])});
        }
        out << '}';
    }
    out << "]}\n";
    out.close();
    return out.good();
}

bool writeScreenshot(const std::filesystem::path& path, const ar::rhi::FrameScreenshot& image) {
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

std::optional<std::vector<std::uint32_t>> readShader(const std::filesystem::path& path) {
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

} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto options = parse(argc, argv);
    if (!options) {
        std::fputs(
            "usage: ar_triangle [--frames N] [--hidden] [--clear-only] "
            "[--set-binding] [--static-scene JSON --asset-root DIRECTORY] "
            "[--gbuffer [--gbuffer-probes PATH]] [--hdr-lighting [--exposure-ev EV]] "
            "[--screenshot PATH] [--output DIRECTORY]\n",
            stderr);
        return 2;
    }

    ar::runtime::RuntimeConfig config;
    config.mode = ar::runtime::QueryMode::Development;
    config.query.requestedApi = ar::rhi::makeApiVersion(1, 3);
    config.query.createGraphicsDevice = true;
    config.query.enableScreenshot =
        options->screenshot.has_value() || options->gbufferProbes.has_value() ||
        options->verifyBindingRetirement;
    config.query.enableSetBinding = options->setBinding;
    config.query.enableSetBindingSample = options->setBinding;
    config.query.verifyBindingRetirement = options->verifyBindingRetirement;
    config.query.enableGBuffer = options->gbuffer;
    config.staticAssetRoot = options->assetRoot;
    if (options->verifyBindingRetirement) {
        config.diagnostics.minLevel = ar::core::LogLevel::Debug;
    }
    if (!options->clearOnly) {
        const std::filesystem::path shaderDir = AR_TRIANGLE_SHADER_DIR;
        auto vertex = readShader(
            shaderDir / (options->gbuffer      ? "gbuffer.vert.spv"
                         : options->scene      ? "static_scene.vert.spv"
                         : options->gpuDriven  ? "gpu_draw.vert.spv"
                         : options->setBinding ? "set_binding.vert.spv"
                                               : "triangle.vert.spv"));
        auto fragment = readShader(
            shaderDir / (options->gbuffer      ? "gbuffer.frag.spv"
                         : options->scene      ? "static_scene.frag.spv"
                         : options->gpuDriven  ? "gpu_draw.frag.spv"
                         : options->setBinding ? "set_binding.frag.spv"
                                               : "triangle.frag.spv"));
        if (options->hdrLighting) {
            auto fullscreen = readShader(shaderDir / "fullscreen.vert.spv");
            auto direct = readShader(shaderDir / "deferred_direct.frag.spv");
            auto display = readShader(shaderDir / "display_sdr.frag.spv");
            if (!fullscreen || !direct || !display) {
                return 1;
            }
            config.query.deferredLighting.emplace(ar::rhi::DeferredLighting{
                std::move(*fullscreen), std::move(*direct), std::move(*display),
                options->exposureEV.value_or(0.F)});
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
    config.window->title = "AR Engine Triangle";
    config.window->visible = options->visible;
    config.diagnostics.outputRoot = options->output;
    const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    config.diagnostics.runId = "triangle-" + std::to_string(timestamp);

    auto session = ar::runtime::RuntimeSession::start(config);
    if (!session) {
        const auto& error = session.error().endResult.primaryError;
        if (error) {
            printError(*error);
        }
        return 1;
    }
    if (options->scene) {
        if (auto loaded = (*session)->openStaticScene(*options->scene); !loaded) {
            printError(loaded.error());
            return 1;
        }
    }

    unsigned long long presented = 0;
    ar::rhi::FrameScreenshot screenshot;
    while (!options->frames || presented < *options->frames) {
        const bool capture = options->sceneCandidate ||
            ((options->screenshot || options->gbufferProbes) && presented + 1 == *options->frames);
        const auto frame = (*session)->presentFrame(capture ? &screenshot : nullptr);
        if (!frame || *frame == ar::runtime::FrameStatus::Closed) {
            break;
        }
        if (*frame == ar::runtime::FrameStatus::Presented) {
            ++presented;
            if (options->sceneCandidate && presented < 4) {
                constexpr std::array names{"before.png", "rollback.png", "reopened.png"};
                const auto directory = options->screenshot->parent_path();
                if (!writeScreenshot(directory / names[presented - 1], screenshot)) {
                    return 1;
                }
                std::expected<void, ar::core::Error> change;
                if (presented == 1) {
                    auto rejected = (*session)->openStaticScene(*options->sceneCandidate, true);
                    if (rejected || rejected.error().message != "injected failure after first candidate mesh upload") {
                        std::fputs("expected failure after first candidate mesh upload\n", stderr);
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
                    return 1;
                }
            }
        }
    }
    const ar::core::EndResult result = (*session)->close();
    std::printf("presented_frames=%llu\n", presented);
    if (result.primaryError) {
        printError(*result.primaryError);
    }
    for (const auto& error : result.cleanupErrors) {
        printError(error);
    }
    for (const auto& error : result.diagnosticErrors) {
        printError(error);
    }
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
