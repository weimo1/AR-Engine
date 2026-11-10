#include "runtime/RuntimeSession.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

#include "CapabilityJson.h"
#include "core/BuildInfo.h"
#include "core/Diagnostics.h"
#include "core/Error.h"
#include "rhi/Capabilities.h"
#include "rhi/RhiSession.h"
#include "runtime/RuntimeConfig.h"

namespace ar::runtime {
namespace {

constexpr std::uint32_t kApiVersion1_3 = rhi::makeApiVersion(1, 3);

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
    if (config.query.presentation || (config.window && (!config.query.createGraphicsDevice ||
                                                        config.mode != QueryMode::Development))) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "runtime presentation requires a runtime window and a development graphics device");
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
        auto extensions = window->requiredInstanceExtensions();
        if (!extensions) {
            return std::unexpected(std::move(extensions.error()));
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
            std::move(*extensions),
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
        const core::Error unsupported = makeError(
            core::ErrorCategory::ApiVersionUnsupported,
            core::ErrorStage::Capability,
            "require_product needs a met 1.4 product target");

        const rhi::CloseResult closeResult = session->rhi->close();
        session->rhi.reset();
        session->closed = true;

        core::RunSummaryInput summary;
        summary.runId = config.diagnostics.runId;
        summary.identity = identityFor(build);
        summary.primaryResult = core::ResultKind::Unsupported;
        summary.primaryError = unsupported;
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

std::expected<bool, core::Error> RuntimeSession::presentClearFrame() {
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
        return false;
    }
    const auto size = window.framebufferSize();
    if (window.isMinimized() || size.width <= 0 || size.height <= 0) {
        m_impl->frameError = makeError(
            core::ErrorCategory::InitializationFailed,
            core::ErrorStage::Capability,
            "the window no longer has a drawable framebuffer");
    } else if (auto presented = m_impl->rhi->presentClearFrame(); !presented) {
        m_impl->frameError = std::move(presented.error());
    }
    if (m_impl->frameError) {
        return std::unexpected(*m_impl->frameError);
    }
    return true;
}

core::EndResult RuntimeSession::close() {
    if (m_impl->closed) {
        return m_impl->cachedClose;
    }

    rhi::CloseResult closeResult;
    if (m_impl->rhi) {
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
