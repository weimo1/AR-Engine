#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "AppPaths.h"
#include "render/CullReference.h"
#include "render/EnvironmentLighting.h"
#include "render/FreeCameraController.h"
#include "runtime/RuntimeSession.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace {

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

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto exeDir = ar::app::currentExecutableDirectory();
    if (!exeDir) {
        std::fprintf(stderr, "%s\n", exeDir.error().c_str());
        return 1;
    }
    const auto paths = ar::app::resolveAppPaths(
        *exeDir, std::nullopt, std::filesystem::path(AR_TRIANGLE_OUTPUT_ROOT));
    if (!paths) {
        std::fprintf(stderr, "%s\n", paths.error().c_str());
        return 1;
    }

    std::vector<std::wstring_view> args;
    if (argc > 1) {
        args.reserve(static_cast<std::size_t>(argc - 1));
        for (int index = 1; index < argc; ++index) {
            args.emplace_back(argv[index]);
        }
    }
    const auto options = ar::app::parseLaunchOptions(args, *paths);
    if (!options) {
        const auto usage = ar::app::launchUsageText();
        std::fwrite(usage.data(), 1, usage.size(), stderr);
        return 2;
    }

    std::optional<std::filesystem::path> sceneFile;
    std::optional<std::filesystem::path> assetDir;
    if (options->scene) {
        auto checkedScene = ar::app::requireReadableFile(*options->scene, "scene file");
        if (!checkedScene) {
            std::fprintf(stderr, "%s\n", checkedScene.error().c_str());
            return 1;
        }
        auto checkedAssets = ar::app::requireReadableDirectory(*options->assetRoot, "asset root");
        if (!checkedAssets) {
            std::fprintf(stderr, "%s\n", checkedAssets.error().c_str());
            return 1;
        }
        sceneFile = std::move(*checkedScene);
        assetDir = std::move(*checkedAssets);
    }

    ar::runtime::RuntimeConfig config;
    config.mode = options->descriptorHeap ? ar::runtime::QueryMode::RequireProduct
                                          : ar::runtime::QueryMode::Development;
    config.query.requestedApi =
        options->descriptorHeap ? ar::rhi::kApiVersion1_4 : ar::rhi::makeApiVersion(1, 3);
    config.query.createGraphicsDevice = true;
    config.query.enableScreenshot =
        options->screenshot.has_value() || options->gbufferProbes.has_value() ||
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
        const auto checkedShaderDir =
            ar::app::requireReadableDirectory(paths->shaders, "shader directory");
        if (!checkedShaderDir) {
            std::fprintf(stderr, "%s\n", checkedShaderDir.error().c_str());
            return 1;
        }
        const std::filesystem::path& shaderDir = *checkedShaderDir;
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
    config.window->title = options->showroom ? "AR Engine Showroom" : "AR Engine Triangle";
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
    if (sceneFile && assetDir) {
        const std::string sceneUtf8 = pathToUtf8(*sceneFile);
        const std::string assetUtf8 = pathToUtf8(*assetDir);
        std::printf(
            "scene=%s\nasset_root=%s\nbinding=%s\nshadows=%s\nfree_camera=%s\n",
            sceneUtf8.c_str(),
            assetUtf8.c_str(),
            options->descriptorHeap ? "descriptor-heap" : "set",
            options->shadows ? "on" : "off",
            options->freeCamera ? "on" : "off");
        if (options->shadows) {
            std::printf("shadow_resolution=%u\n", options->shadowResolution.value_or(2048u));
        }
        std::fflush(stdout);
    }

    std::optional<ar::render::FreeCameraController> freeCamera;
    ar::platform::CursorMode cursorMode = ar::platform::CursorMode::Normal;
    auto lastCameraTick = std::chrono::steady_clock::now();
    if (options->freeCamera) {
        const auto initialCamera = (*session)->activeCamera();
        if (!initialCamera) {
            std::fputs("free camera requires an active scene camera\n", stderr);
            closeAndReport(**session);
            return 1;
        }
        auto controller = ar::render::FreeCameraController::create(*initialCamera);
        if (!controller) {
            printError(controller.error());
            closeAndReport(**session);
            return 1;
        }
        freeCamera = std::move(*controller);
    }

    unsigned long long presented = 0;
    ar::rhi::FrameScreenshot screenshot;
    while (!options->frames || presented < *options->frames) {
        if (freeCamera) {
            const auto now = std::chrono::steady_clock::now();
            const float deltaSeconds = std::chrono::duration<float>(now - lastCameraTick).count();
            lastCameraTick = now;

            const auto input = (*session)->sampleWindowInput();
            if (!input) {
                printError(input.error());
                closeAndReport(**session);
                return 1;
            }
            if (input->escapePressed) {
                break;
            }
            const auto desiredCursor = (input->focused && input->lookHeld)
                                           ? ar::platform::CursorMode::Disabled
                                           : ar::platform::CursorMode::Normal;
            if (desiredCursor != cursorMode) {
                if (auto changed = (*session)->setCursorMode(desiredCursor); !changed) {
                    printError(changed.error());
                    closeAndReport(**session);
                    return 1;
                }
                cursorMode = desiredCursor;
            }
            const ar::render::FreeCameraInput cameraInput{
                input->moveForward,
                input->moveBackward,
                input->moveLeft,
                input->moveRight,
                input->moveUp,
                input->moveDown,
                input->sprint,
                input->focused && input->lookHeld,
                static_cast<float>(input->cursorDeltaX),
                static_cast<float>(input->cursorDeltaY),
                static_cast<float>(input->scrollDeltaY)};
            const auto nextCamera = freeCamera->update(cameraInput, deltaSeconds);
            if (auto updated = (*session)->setCamera(nextCamera); !updated) {
                printError(updated.error());
                closeAndReport(**session);
                return 1;
            }
        }
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
    if (cursorMode != ar::platform::CursorMode::Normal) {
        (void)(*session)->setCursorMode(ar::platform::CursorMode::Normal);
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
