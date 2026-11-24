#include "runtime/RuntimeSession.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

#include "CapabilityJson.h"
#include "core/BuildInfo.h"
#include "core/Diagnostics.h"
#include "core/Error.h"
#include "rhi/Capabilities.h"
#include "rhi/RhiSession.h"
#include "render/DirectionalShadow.h"
#include "render/PresentationGraph.h"
#include "render/PresentationSpec.h"
#include "render/StaticSceneRenderer.h"
#include "runtime/RuntimeConfig.h"
#include "runtime/StaticSceneSession.h"

namespace ar::runtime {
namespace {

constexpr std::uint32_t kApiVersion1_3 = rhi::makeApiVersion(1, 3);
constexpr double kWindowEventWaitSeconds = 0.05;

core::ResultKind resultKindFor(core::ErrorCategory category) noexcept {
    switch (category) {
    case core::ErrorCategory::InvalidArguments:
    case core::ErrorCategory::InvalidState:
        return core::ResultKind::InvalidArguments;
    case core::ErrorCategory::ApiVersionUnsupported:
    case core::ErrorCategory::ApiVariantUnsupported:
    case core::ErrorCategory::RequestedApiUnsupported:
        return core::ResultKind::Unsupported;
    case core::ErrorCategory::OutputFailed:
        return core::ResultKind::OutputFailed;
    default:
        return core::ResultKind::InitializationFailed;
    }
}

core::Error makeError(core::ErrorCategory category, core::ErrorStage stage, std::string message) {
    return core::Error{category, stage, std::move(message), std::nullopt};
}

// Requested API and mode must agree before any OS load happens, so a mismatch never touches the
// loader or writes a diagnostic run.
std::optional<core::Error> validateModeRequest(const RuntimeConfig& config) {
    const std::uint32_t requested = config.query.requestedApi;
    const bool presentationModeAllowed =
        config.mode == QueryMode::Development || config.mode == QueryMode::RequireProduct;
    if (config.query.presentation ||
        (config.window && (!config.query.createGraphicsDevice || !presentationModeAllowed))) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "runtime presentation requires a runtime window and a development or require_product "
            "graphics device");
    }
    switch (config.mode) {
    case QueryMode::Inspect:
        if (requested != kApiVersion1_3 && requested != rhi::kApiVersion1_4) {
            return makeError(
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Validation,
                "inspect mode accepts a requested API of 1.3 or 1.4");
        }
        return std::nullopt;
    case QueryMode::RequireProduct:
        if (requested != rhi::kApiVersion1_4) {
            return makeError(
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Validation,
                "require_product mode requires a requested API of 1.4");
        }
        return std::nullopt;
    case QueryMode::Development:
        if (requested != kApiVersion1_3) {
            return makeError(
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Validation,
                "development mode requires an explicit requested API of 1.3");
        }
        return std::nullopt;
    }
    return makeError(
        core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation, "unknown query mode");
}

core::EndResult rejectedStart(const core::Error& error) {
    core::EndResult result;
    const core::ResultKind kind = resultKindFor(error.category);
    result.result = kind;
    result.primaryResult = kind;
    result.primaryError = error;
    result.diagnosticsComplete = false;
    result.resourcesReleased = true;
    return result;
}

core::RunSummaryIdentity identityFor(const core::BuildInfo& build) {
    return core::RunSummaryIdentity{
        build.buildId, build.inputHash, build.configuration, build.source};
}

} // namespace

struct RuntimeSession::Impl {
    RuntimeConfig config;
    std::unique_ptr<core::DiagnosticSession> diagnostics;
    std::unique_ptr<platform::Window> window;
    std::unique_ptr<rhi::RhiSession> rhi;
    std::unique_ptr<StaticSceneSession> staticScene;
    std::optional<render::StaticCamera> activeCamera;
    std::optional<render::SceneWorldBounds> activeSceneBounds;
    std::optional<render::EffectiveDirectionalLight> activeDirectionalLight;
    std::uint64_t staticSceneUploadCount = 0;

    std::expected<rhi::DirectionalLightingState, core::Error> buildLightingState(
        const render::EffectiveDirectionalLight& effective,
        const render::SceneWorldBounds& bounds) const {
        rhi::DirectionalLightingState state{};
        if (effective.legacyViewSpace) {
            return state;
        }
        state.legacyViewSpace = false;
        state.directionWorld = effective.directionWorld;
        state.color = effective.color;
        state.intensity = effective.intensity;
        state.shadowEnabled = effective.castShadows;
        if (config.query.deferredLighting && config.query.deferredLighting->shadow) {
            const auto& shadowCfg = *config.query.deferredLighting->shadow;
            state.normalBias = shadowCfg.normalBias;
            state.depthBias = shadowCfg.depthBias;
            state.pcfRadiusTexels = shadowCfg.pcfRadiusTexels;
            if (effective.castShadows) {
                const render::DirectionalLight light{
                    effective.directionWorld, effective.color, effective.intensity, true};
                const render::DirectionalShadowConfig shadowConfig{
                    shadowCfg.resolution,
                    shadowCfg.normalBias,
                    shadowCfg.depthBias,
                    shadowCfg.pcfRadiusTexels};
                auto shadowView = render::buildDirectionalShadowView(bounds, light, shadowConfig);
                if (!shadowView) {
                    return std::unexpected(std::move(shadowView.error()));
                }
                state.lightViewProj = shadowView->lightViewProj;
            }
        }
        return state;
    }
    // Runtime-owned snapshot: survives close and never aliases the rhi report.
    rhi::CapabilityReport report;
    std::optional<core::Error> frameError;
    bool closed = false;
    core::EndResult cachedClose;

    std::expected<void, core::Error> openWindow(rhi::QueryConfig& query) {
        if (!config.window) {
            return {};
        }
        auto opened = platform::Window::open(*config.window);
        if (!opened) {
            return std::unexpected(std::move(opened.error()));
        }
        window = std::move(*opened);
        std::vector<std::string> extensions;
        if (std::holds_alternative<rhi::ExplicitLoaderPath>(query.loader)) {
            extensions = {"VK_KHR_surface", "VK_KHR_win32_surface"};
        } else {
            auto required = window->requiredInstanceExtensions();
            if (!required) {
                return std::unexpected(std::move(required.error()));
            }
            extensions = std::move(*required);
        }
        const auto size = window->framebufferSize();
        if (size.width <= 0 || size.height <= 0) {
            return std::unexpected(makeError(
                core::ErrorCategory::InitializationFailed,
                core::ErrorStage::Capability,
                "window has no drawable framebuffer"));
        }
        query.presentation = rhi::Win32Presentation{
            window->nativeWin32Handle(),
            std::move(extensions),
            static_cast<std::uint32_t>(size.width),
            static_cast<std::uint32_t>(size.height)};
        return {};
    }

    void closeWindow(const std::vector<core::ResidualResource>& retained) noexcept {
        if (std::ranges::any_of(retained, [](const auto& resource) {
                return resource.kind == core::ResourceKind::Presentation;
            })) {
            // The surface still uses the HWND. The quarantined process keeps it until exit.
            (void)window.release();
        } else {
            window.reset();
        }
    }
};

RuntimeSession::RuntimeSession(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

RuntimeSession::~RuntimeSession() {
    // Fallback release keeps window and diagnostics alive until RHI is destroyed.
    if (m_impl->window && m_impl->rhi) {
        m_impl->closeWindow(m_impl->rhi->close().residualResources);
    }
}

std::expected<std::unique_ptr<RuntimeSession>, StartFailure>
RuntimeSession::start(const RuntimeConfig& config) {
    if (const std::optional<core::Error> invalid = validateModeRequest(config);
        invalid.has_value()) {
        StartFailure failure;
        failure.endResult = rejectedStart(*invalid);
        // No rhi session exists here, so the report is a default value; its result must still
        // describe the rejection instead of claiming a successful query.
        failure.report.result = resultKindFor(invalid->category);
        return std::unexpected(std::move(failure));
    }

    const core::BuildInfo& build = core::buildInfo();
    auto diagnostics = core::DiagnosticSession::create(config.diagnostics, identityFor(build));
    if (!diagnostics.has_value()) {
        StartFailure failure;
        failure.endResult = rejectedStart(diagnostics.error());
        failure.report.result = resultKindFor(diagnostics.error().category);
        return std::unexpected(std::move(failure));
    }

    auto session = std::make_unique<Impl>();
    session->config = config;
    session->diagnostics = std::move(*diagnostics);

    auto query = config.query;
    if (query.enableGBuffer) {
        if (!query.planPresentation) {
            query.planPresentation = render::planPresentationGraph;
        }
        if (!query.presentationSpec) {
            const float exposureEV =
                query.deferredLighting ? query.deferredLighting->exposureEV : 0.F;
            query.presentationSpec = render::buildDeferredPresentationSpec(exposureEV);
        }
    }
    if (config.staticAssetRoot) {
        query.enableStaticScene = true;
        session->staticScene = std::make_unique<StaticSceneSession>(
            *config.staticAssetRoot, query.enableGBuffer);
    }
    auto rhi = [&]() -> std::expected<std::unique_ptr<rhi::RhiSession>, rhi::QueryFailure> {
        if (auto opened = session->openWindow(query); !opened) {
            rhi::QueryFailure failure;
            failure.error = std::move(opened.error());
            failure.report.result = resultKindFor(failure.error.category);
            return std::unexpected(std::move(failure));
        }
        return rhi::RhiSession::open(query, *session->diagnostics);
    }();
    if (!rhi.has_value()) {
        const rhi::QueryFailure& failure = rhi.error();
        core::RunSummaryInput summary;
        summary.runId = config.diagnostics.runId;
        summary.identity = identityFor(build);
        summary.primaryResult = resultKindFor(failure.error.category);
        summary.primaryError = failure.error;
        summary.cleanupErrors = failure.cleanupErrors;
        summary.resourcesReleased = failure.cleanupErrors.empty();
        // A rollback that retained an instance or module must survive into the summary; otherwise
        // the failed start looks released while the process is quarantined.
        summary.residualResources = failure.residualResources;
        session->report = failure.report;
        session->closeWindow(failure.residualResources);
        session->closed = true;

        StartFailure startFailure;
        startFailure.endResult = session->diagnostics->finish(summary);
        startFailure.report = failure.report;
        startFailure.diagnosticsAvailable = true;
        return std::unexpected(std::move(startFailure));
    }

    session->rhi = std::move(*rhi);
    session->report = session->rhi->report();

    // require_product is the only mode that turns a complete but insufficient query into a
    // failed start. inspect and development keep the product verdict inside the report.
    if (config.mode == QueryMode::RequireProduct && !session->report.productTargetSupported) {
        const bool apiUnsupported = !session->report.judgments.apiVersionSupported;
        const core::Error unmet = makeError(
            apiUnsupported ? core::ErrorCategory::ApiVersionUnsupported
                           : core::ErrorCategory::QueryFailed,
            core::ErrorStage::Capability,
            apiUnsupported ? "require_product needs a met 1.4 product target"
                           : "require_product did not satisfy required Vulkan 1.4 capabilities");

        const rhi::CloseResult closeResult = session->rhi->close();
        session->rhi.reset();
        session->closeWindow(closeResult.residualResources);
        session->closed = true;

        core::RunSummaryInput summary;
        summary.runId = config.diagnostics.runId;
        summary.identity = identityFor(build);
        summary.primaryResult = resultKindFor(unmet.category);
        summary.primaryError = unmet;
        summary.cleanupErrors = closeResult.errors;
        summary.resourcesReleased = closeResult.resourcesReleased;
        summary.residualResources = closeResult.residualResources;
        const detail::EncodedCapability encoded =
            detail::encodeCapability(session->report, config.mode, config.diagnostics.runId, build);
        summary.capabilityJson = encoded.json;
        summary.capabilityEncodingError = encoded.error;

        StartFailure failure;
        failure.endResult = session->diagnostics->finish(summary);
        failure.report = session->report;
        failure.diagnosticsAvailable = true;
        return std::unexpected(std::move(failure));
    }

    return std::unique_ptr<RuntimeSession>(new RuntimeSession(std::move(session)));
}

const rhi::CapabilityReport& RuntimeSession::report() const noexcept {
    return m_impl->report;
}

std::expected<FrameStatus, core::Error>
RuntimeSession::presentFrame(rhi::FrameScreenshot* screenshot) {
    if (m_impl->closed || !m_impl->window) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidState,
            core::ErrorStage::Capability,
            "this runtime session has no active window"));
    }
    if (m_impl->frameError) {
        return std::unexpected(*m_impl->frameError);
    }
    auto& window = *m_impl->window;
    window.pollEvents();
    if (window.shouldClose()) {
        return FrameStatus::Closed;
    }
    const auto size = window.framebufferSize();
    if (window.isMinimized() || size.width <= 0 || size.height <= 0) {
        window.waitEvents(kWindowEventWaitSeconds);
        return window.shouldClose() ? FrameStatus::Closed : FrameStatus::Paused;
    }
    const auto presented = m_impl->rhi->presentFrame(
        static_cast<std::uint32_t>(size.width),
        static_cast<std::uint32_t>(size.height),
        screenshot);
    if (!presented) {
        m_impl->frameError = std::move(presented.error());
        return std::unexpected(*m_impl->frameError);
    }
    if (*presented == rhi::PresentStatus::Retry) {
        window.waitEvents(kWindowEventWaitSeconds);
        return window.shouldClose() ? FrameStatus::Closed : FrameStatus::Paused;
    }
    if (screenshot && screenshot->rawProbes) {
        std::optional<render::StaticCamera> camera = m_impl->activeCamera;
        if (!camera && m_impl->staticScene) {
            if (const auto input = m_impl->staticScene->input()) {
                camera = input->camera;
            }
        }
        screenshot->gbuffer = render::decodeGBufferProbes(
            *screenshot->rawProbes,
            screenshot->width,
            screenshot->height,
            camera ? &*camera : nullptr);
    }
    return FrameStatus::Presented;
}

std::expected<void, core::Error>
RuntimeSession::openStaticScene(const std::filesystem::path& source, bool verifyRollback) {
    if (m_impl->closed || !m_impl->staticScene) {
        return std::unexpected(makeError(core::ErrorCategory::InvalidState,
            core::ErrorStage::Capability, "no active static scene session"));
    }
    std::optional<core::Error> gpuError;
    std::optional<render::SceneWorldBounds> pendingBounds;
    std::optional<render::EffectiveDirectionalLight> pendingLight;
    auto opened = m_impl->staticScene->open(
        source, [&](const render::StaticRenderInput& input) -> std::expected<void, StaticSceneError> {
            auto bounds = render::computeSceneWorldBounds(input);
            if (!bounds) {
                gpuError = std::move(bounds.error());
                return std::unexpected(
                    StaticSceneError{StaticSceneStage::Asset, source, gpuError->message});
            }
            auto effective = render::resolveDirectionalLight(
                input.directionalLight, m_impl->rhi->shadowMappingEnabled());
            if (!effective) {
                gpuError = std::move(effective.error());
                return std::unexpected(
                    StaticSceneError{StaticSceneStage::Asset, source, gpuError->message});
            }
            std::optional<rhi::DirectionalLightingState> lightingState;
            if (m_impl->config.query.deferredLighting) {
                auto built = m_impl->buildLightingState(*effective, *bounds);
                if (!built) {
                    gpuError = std::move(built.error());
                    return std::unexpected(
                        StaticSceneError{StaticSceneStage::Asset, source, gpuError->message});
                }
                lightingState = *built;
            }
            auto ready = render::publishStaticScene(
                *m_impl->rhi, input, verifyRollback, m_impl->config.query.enableGBuffer);
            if (!ready) {
                gpuError = std::move(ready.error());
                return std::unexpected(
                    StaticSceneError{StaticSceneStage::Asset, source, gpuError->message});
            }
            if (lightingState) {
                if (auto applied = m_impl->rhi->setDirectionalLighting(*lightingState); !applied) {
                    gpuError = std::move(applied.error());
                    return std::unexpected(
                        StaticSceneError{StaticSceneStage::Asset, source, gpuError->message});
                }
            }
            pendingBounds = *bounds;
            pendingLight = *effective;
            return {};
        });
    if (!opened) {
        if (gpuError) {
            return std::unexpected(std::move(*gpuError));
        }
        const auto path = opened.error().source.u8string();
        return std::unexpected(makeError(core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation, std::string(path.begin(), path.end()) +
                ": " + opened.error().message));
    }
    if (const auto input = m_impl->staticScene->input()) {
        m_impl->activeCamera = input->camera;
    }
    m_impl->activeSceneBounds = pendingBounds;
    m_impl->activeDirectionalLight = pendingLight;
    return {};
}

std::expected<void, core::Error> RuntimeSession::setCamera(const render::StaticCamera& camera) {
    if (m_impl->closed || !m_impl->rhi || !m_impl->staticScene) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidState,
            core::ErrorStage::Capability,
            "no active static scene session"));
    }
    auto frameView = render::buildFrameView(camera, m_impl->config.query.enableGBuffer);
    if (!frameView) {
        return std::unexpected(std::move(frameView.error()));
    }
    if (auto updated = m_impl->rhi->setFrameView(*frameView); !updated) {
        return updated;
    }
    m_impl->activeCamera = camera;
    return {};
}

std::expected<void, core::Error>
RuntimeSession::setDirectionalLight(const render::DirectionalLight& light) {
    if (m_impl->closed || !m_impl->rhi || !m_impl->staticScene ||
        !m_impl->config.query.deferredLighting) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidState,
            core::ErrorStage::Capability,
            "deferred lighting session is not active"));
    }
    if (auto valid = render::validateDirectionalLight(light); !valid) {
        return valid;
    }
    const bool shadowPipelineEnabled = m_impl->rhi->shadowMappingEnabled();
    if (!shadowPipelineEnabled && light.castShadows) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidState,
            core::ErrorStage::Capability,
            "directional shadows require the shadow pipeline to be enabled at startup"));
    }
    auto effective = render::resolveDirectionalLight(light, shadowPipelineEnabled);
    if (!effective) {
        return std::unexpected(std::move(effective.error()));
    }
    const render::SceneWorldBounds bounds =
        m_impl->activeSceneBounds.value_or(render::SceneWorldBounds{});
    auto state = m_impl->buildLightingState(*effective, bounds);
    if (!state) {
        return std::unexpected(std::move(state.error()));
    }
    if (auto applied = m_impl->rhi->setDirectionalLighting(*state); !applied) {
        return applied;
    }
    m_impl->activeDirectionalLight = *effective;
    return {};
}

std::uint64_t RuntimeSession::staticSceneUploadCount() const noexcept {
    return m_impl->rhi ? m_impl->rhi->staticSceneUploadCount() : m_impl->staticSceneUploadCount;
}

std::expected<void, core::Error>
RuntimeSession::replaceEnvironment(const rhi::EnvironmentUpload& upload, bool verifyRollback) {
    if (!m_impl->rhi || m_impl->closed) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "no active render session",
                {}});
    }
    return m_impl->rhi->replaceEnvironment(upload, verifyRollback);
}

std::expected<void, core::Error>
RuntimeSession::saveStaticScene(const std::filesystem::path& destination) const {
    if (m_impl->closed || !m_impl->staticScene) {
        return std::unexpected(makeError(core::ErrorCategory::InvalidState,
            core::ErrorStage::Capability, "no active static scene session"));
    }
    if (auto saved = m_impl->staticScene->save(destination); !saved) {
        return std::unexpected(makeError(core::ErrorCategory::OutputFailed,
            core::ErrorStage::Output, saved.error().message));
    }
    return {};
}

core::EndResult RuntimeSession::close() {
    if (m_impl->closed) {
        return m_impl->cachedClose;
    }

    rhi::CloseResult closeResult;
    if (m_impl->rhi) {
        m_impl->staticSceneUploadCount = m_impl->rhi->staticSceneUploadCount();
        closeResult = m_impl->rhi->close();
        m_impl->rhi.reset();
    }
    m_impl->closeWindow(closeResult.residualResources);

    const core::BuildInfo& build = core::buildInfo();
    core::RunSummaryInput summary;
    summary.runId = m_impl->config.diagnostics.runId;
    summary.identity = identityFor(build);
    summary.primaryError = m_impl->frameError;
    summary.primaryResult = summary.primaryError ? resultKindFor(summary.primaryError->category)
                                                 : core::ResultKind::Success;
    summary.cleanupErrors = closeResult.errors;
    summary.resourcesReleased = closeResult.resourcesReleased;
    summary.residualResources = closeResult.residualResources;

    // A complete query always produces an embedded capability object; an encoding failure is
    // reported explicitly instead of silently passing nullopt.
    const detail::EncodedCapability encoded = detail::encodeCapability(
        m_impl->report, m_impl->config.mode, m_impl->config.diagnostics.runId, build);
    summary.capabilityJson = encoded.json;
    summary.capabilityEncodingError = encoded.error;

    m_impl->cachedClose = m_impl->diagnostics->finish(summary);
    m_impl->closed = true;
    return m_impl->cachedClose;
}

} // namespace ar::runtime
