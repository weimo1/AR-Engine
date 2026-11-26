#include "AppPaths.h"
#include "BenchmarkRun.h"
#include "DebugPanel.h"
#include "core/BuildInfo.h"
#include "render/DirectionalShadow.h"
#include "render/EnvironmentLighting.h"
#include "render/FreeCameraController.h"
#include "runtime/ObservationSettings.h"
#include "runtime/RuntimeSession.h"

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
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace {

constexpr std::uint32_t kDefaultShadowResolution = 2048u;

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

std::string
sceneReferenceFor(const std::filesystem::path& sceneFile, const std::filesystem::path& contentDir) {
    std::error_code error;
    const auto relative = std::filesystem::relative(sceneFile, contentDir, error);
    if (!error && !relative.empty()) {
        const auto generic = relative.generic_u8string();
        std::string text(generic.begin(), generic.end());
        if (!text.starts_with("..")) {
            return text;
        }
    }
    const auto filename = sceneFile.filename().generic_u8string();
    return std::string(filename.begin(), filename.end());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);
#endif
    const auto exeDir = ar::app::currentExecutableDirectory();
    if (!exeDir) {
        std::fprintf(stderr, "%s\n", exeDir.error().c_str());
        return 1;
    }
    const auto defaultOutput = ar::app::defaultOutputDirectoryFor(*exeDir);
    if (!defaultOutput) {
        std::fprintf(stderr, "%s\n", defaultOutput.error().c_str());
        return 1;
    }
    const auto paths = ar::app::resolveAppPaths(*exeDir, std::nullopt, *defaultOutput);
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

    if (options->benchmarkSpec) {
        const auto benchConfig = ar::app::loadBenchmarkConfig(*options->benchmarkSpec, *paths);
        if (!benchConfig) {
            std::fprintf(stderr, "%s\n", benchConfig.error().c_str());
            return 2;
        }
        return ar::app::runBenchmark(*benchConfig, *paths);
    }

    const auto sceneFile = ar::app::requireReadableFile(options->scene, "scene file");
    if (!sceneFile) {
        std::fprintf(stderr, "%s\n", sceneFile.error().c_str());
        return 1;
    }
    const auto assetDir = ar::app::requireReadableDirectory(options->assetRoot, "asset root");
    if (!assetDir) {
        std::fprintf(stderr, "%s\n", assetDir.error().c_str());
        return 1;
    }
    const auto shaderDir = ar::app::requireReadableDirectory(paths->shaders, "shader directory");
    if (!shaderDir) {
        std::fprintf(stderr, "%s\n", shaderDir.error().c_str());
        return 1;
    }
    const auto writableOutput =
        ar::app::verifyWritableDirectory(options->output, "output directory");
    if (!writableOutput) {
        std::fprintf(stderr, "%s\n", writableOutput.error().c_str());
        return 1;
    }

    const bool interactive = options->visible;
    auto gbufferVert = readShader(*shaderDir / "gbuffer.vert.spv");
    auto gbufferFrag = readShader(*shaderDir / "gbuffer_bindless.frag.spv");
    auto indirectVert = readShader(*shaderDir / "static_indirect.vert.spv");
    auto indirectFrag = readShader(*shaderDir / "static_indirect.frag.spv");
    auto indirectComp = readShader(*shaderDir / "static_indirect.comp.spv");
    auto fullscreenVert = readShader(*shaderDir / "fullscreen.vert.spv");
    auto lightingFrag = readShader(*shaderDir / "deferred_ibl_shadow.frag.spv");
    auto displayFrag = readShader(*shaderDir / "display_sdr.frag.spv");
    auto shadowDirectVert = readShader(*shaderDir / "shadow_direct.vert.spv");
    auto shadowIndirectVert = readShader(*shaderDir / "shadow_indirect.vert.spv");
    if (!gbufferVert || !gbufferFrag || !indirectVert || !indirectFrag || !indirectComp ||
        !fullscreenVert || !lightingFrag || !displayFrag || !shadowDirectVert ||
        !shadowIndirectVert) {
        return 1;
    }

    std::optional<ar::rhi::OverlayShaders> overlayShaders;
    if (interactive) {
        auto overlayVert = readShader(*shaderDir / "overlay.vert.spv");
        auto overlayFrag = readShader(*shaderDir / "overlay.frag.spv");
        if (!overlayVert || !overlayFrag) {
            return 1;
        }
        overlayShaders.emplace(
            ar::rhi::OverlayShaders{std::move(*overlayVert), std::move(*overlayFrag)});
    }

    ar::runtime::RuntimeConfig config;
    config.mode = options->descriptorHeap ? ar::runtime::QueryMode::RequireProduct
                                          : ar::runtime::QueryMode::Development;
    config.query.requestedApi =
        options->descriptorHeap ? ar::rhi::kApiVersion1_4 : ar::rhi::makeApiVersion(1, 3);
    config.query.createGraphicsDevice = true;
    config.query.enableDescriptorHeap = options->descriptorHeap;
    config.query.enableSetBinding = !options->descriptorHeap;
    config.query.enableGBuffer = true;
    config.query.enableScreenshot =
        options->screenshot.has_value() || options->gbufferProbes.has_value();
    config.staticAssetRoot = *assetDir;

    config.query.triangleShaders =
        ar::rhi::TriangleShaders{std::move(*gbufferVert), std::move(*gbufferFrag)};
    config.query.staticIndirect.emplace(
        ar::rhi::StaticIndirectShaders{
            {std::move(*indirectVert), std::move(*indirectFrag)},
            std::move(*indirectComp),
            UINT32_MAX});
    config.query.deferredLighting.emplace(
        ar::rhi::DeferredLighting{
            std::move(*fullscreenVert),
            std::move(*lightingFrag),
            std::move(*displayFrag),
            0.F,
            true,
            ar::rhi::ShadowMapConfig{
                std::move(*shadowDirectVert),
                std::move(*shadowIndirectVert),
                kDefaultShadowResolution}});
    config.query.overlayShaders = std::move(overlayShaders);

    config.window.emplace();
    config.window->title = "AR Engine";
    config.window->visible = options->visible;
    config.diagnostics.outputRoot = options->output;
    const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    config.diagnostics.runId = "engine-" + std::to_string(timestamp);

    auto session = ar::runtime::RuntimeSession::start(config);
    if (!session) {
        reportEndErrors(session.error().endResult);
        return 1;
    }
    if (auto loaded = (*session)->openStaticScene(*sceneFile); !loaded) {
        printError(loaded.error());
        closeAndReport(**session);
        return 1;
    }
    {
        const auto packed = ar::render::packEnvironmentLighting(
            ar::render::bakeEnvironmentLighting(ar::render::EnvironmentPreset::Directional));
        if (auto loaded = (*session)->replaceEnvironment(
                {packed.diffuse, packed.specular, packed.brdf}, false);
            !loaded) {
            printError(loaded.error());
            closeAndReport(**session);
            return 1;
        }
    }

    const std::string sceneRef = sceneReferenceFor(*sceneFile, paths->content);
    const std::filesystem::path presetDir = *writableOutput / "presets";
    const ar::core::BuildInfo& build = ar::core::buildInfo();
    std::printf(
        "build_id=%s\nbuild_config=%s\nscene=%s\nasset_root=%s\noutput_dir=%s\nbinding=%s\n"
        "shadows=on\nshadow_resolution=%u\nfree_camera=%s\n",
        build.buildId.c_str(),
        build.configuration.c_str(),
        pathToUtf8(*sceneFile).c_str(),
        pathToUtf8(*assetDir).c_str(),
        pathToUtf8(*writableOutput).c_str(),
        options->descriptorHeap ? "descriptor-heap" : "set",
        kDefaultShadowResolution,
        interactive ? "on" : "off");
    std::fflush(stdout);

    std::optional<ar::render::FreeCameraController> freeCamera;
    std::optional<ar::render::StaticCamera> initialSceneCamera;
    std::unique_ptr<ar::app::DebugPanel> debugPanel;
    ar::platform::CursorMode cursorMode = ar::platform::CursorMode::Normal;
    auto lastFrameTime = std::chrono::steady_clock::now();
    double lastCpuFrameMs = 0.0;

    if (interactive) {
        initialSceneCamera = (*session)->activeCamera();
        if (!initialSceneCamera) {
            std::fputs("free camera requires an active scene camera\n", stderr);
            closeAndReport(**session);
            return 1;
        }
        auto controller = ar::render::FreeCameraController::create(*initialSceneCamera);
        if (!controller) {
            printError(controller.error());
            closeAndReport(**session);
            return 1;
        }
        freeCamera = std::move(*controller);
        debugPanel = std::make_unique<ar::app::DebugPanel>();
        const auto fontAtlas = debugPanel->buildFontAtlas();
        if (auto uploaded = (*session)->uploadOverlayFont(fontAtlas); !uploaded) {
            printError(uploaded.error());
            closeAndReport(**session);
            return 1;
        }
    }

    unsigned long long presented = 0;
    ar::rhi::FrameScreenshot screenshot;
    while (!options->frames || presented < *options->frames) {
        const auto frameStart = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(frameStart - lastFrameTime).count();
        lastFrameTime = frameStart;

        const bool capture = (options->screenshot || options->gbufferProbes) && options->frames &&
                             presented + 1 == *options->frames;
        std::optional<ar::rhi::OverlayDrawList> overlayDraw;
        bool polledThisFrame = false;

        if (interactive && freeCamera && debugPanel) {
            const auto frameInput = (*session)->pollFrameInput();
            if (!frameInput) {
                printError(frameInput.error());
                closeAndReport(**session);
                return 1;
            }
            polledThisFrame = true;
            if (frameInput->closeRequested) {
                break;
            }
            if (!frameInput->minimized && frameInput->framebufferSize.width > 0 &&
                frameInput->framebufferSize.height > 0) {
                ar::app::PanelSnapshot snap{};
                snap.sceneRef = sceneRef;
                snap.bindingPath = options->descriptorHeap ? "Vulkan 1.4 Descriptor Heap"
                                                           : "Vulkan 1.3 Descriptor Set";
                snap.deviceName = (*session)->report().deviceName;
                snap.instanceCount = (*session)->activeInstanceCount();
                snap.cpuFrameMs = lastCpuFrameMs;
                snap.gpuTimings = (*session)->latestGpuTimings();
                snap.activeCamera = (*session)->activeCamera().value_or(freeCamera->camera());
                snap.activeExposureEV = (*session)->activeExposureEV();
                snap.activeLight = (*session)->activeDirectionalLight().value_or(
                    ar::render::defaultDirectionalLight());
                snap.shadowsSupported = true;
                snap.activeViewMode = (*session)->activeDebugView();
                snap.cameraLooking = (cursorMode == ar::platform::CursorMode::Disabled);
                snap.presetDirectory = presetDir;

                auto actions = debugPanel->tick(deltaSeconds, *frameInput, snap);
                if (auto cap = (*session)->setUiCapture(
                        actions.wantCaptureKeyboard, actions.wantCaptureMouse);
                    !cap) {
                    printError(cap.error());
                    closeAndReport(**session);
                    return 1;
                }

                if (actions.requestedExposureEV) {
                    if (auto ok = (*session)->setExposureEV(*actions.requestedExposureEV); !ok) {
                        debugPanel->setStatus(false, ok.error().message);
                    }
                }
                if (actions.requestedLight) {
                    if (auto ok = (*session)->setDirectionalLight(*actions.requestedLight); !ok) {
                        debugPanel->setStatus(false, ok.error().message);
                    }
                }
                if (actions.requestedViewMode) {
                    if (auto ok = (*session)->setDebugView(*actions.requestedViewMode); !ok) {
                        debugPanel->setStatus(false, ok.error().message);
                    }
                }
                if (actions.requestResetCamera && initialSceneCamera) {
                    if (auto reset = freeCamera->reset(*initialSceneCamera); !reset) {
                        debugPanel->setStatus(false, reset.error().message);
                    } else if (auto ok = (*session)->setCamera(freeCamera->camera()); !ok) {
                        debugPanel->setStatus(false, ok.error().message);
                    } else {
                        debugPanel->setStatus(true, "Camera reset");
                    }
                }
                if (actions.requestSavePreset) {
                    std::error_code dirErr;
                    std::filesystem::create_directories(
                        actions.requestSavePreset->parent_path(), dirErr);
                    auto captured = (*session)->captureObservationSettings("interactive", sceneRef);
                    if (!captured) {
                        debugPanel->setStatus(false, captured.error().message);
                    } else if (auto saved = ar::runtime::saveObservationSettings(
                                   *actions.requestSavePreset, *captured);
                               !saved) {
                        debugPanel->setStatus(false, saved.error().message);
                    } else {
                        debugPanel->setStatus(
                            true, "Saved: " + pathToUtf8(actions.requestSavePreset->filename()));
                    }
                }
                if (actions.requestLoadPreset) {
                    std::filesystem::path loadTarget = *actions.requestLoadPreset;
                    std::error_code probeErr;
                    if (!std::filesystem::is_regular_file(loadTarget, probeErr)) {
                        const std::filesystem::path bundled =
                            paths->presets / loadTarget.filename();
                        probeErr.clear();
                        if (std::filesystem::is_regular_file(bundled, probeErr)) {
                            loadTarget = bundled;
                        }
                    }
                    auto loaded = ar::runtime::loadObservationSettings(loadTarget, sceneRef);
                    if (!loaded) {
                        debugPanel->setStatus(false, loaded.error().message);
                    } else if (auto applied = (*session)->applyObservationSettings(*loaded);
                               !applied) {
                        debugPanel->setStatus(false, applied.error().message);
                    } else {
                        static_cast<void>(freeCamera->reset(loaded->camera));
                        debugPanel->setStatus(true, "Loaded: " + pathToUtf8(loadTarget.filename()));
                    }
                }

                const auto input = (*session)->sampleCameraInput();
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

                if (!capture && actions.panelVisible && !actions.drawList.commands.empty()) {
                    overlayDraw.emplace(std::move(actions.drawList));
                }
            }
        }

        const auto frame = (*session)->presentFrame(
            capture ? &screenshot : nullptr,
            overlayDraw ? &*overlayDraw : nullptr,
            !polledThisFrame);
        if (!frame) {
            printError(frame.error());
            break;
        }
        if (*frame == ar::runtime::FrameStatus::Closed) {
            break;
        }
        if (*frame == ar::runtime::FrameStatus::Presented) {
            ++presented;
            const auto frameEnd = std::chrono::steady_clock::now();
            lastCpuFrameMs =
                std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
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
