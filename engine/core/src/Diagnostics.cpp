#include "core/Diagnostics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "DiagnosticsTestHooks.h"

namespace ar::core {

namespace detail {
#if defined(AR_ENABLE_TEST_HOOKS)
namespace {
std::atomic<std::size_t> g_flushFaults{0};
std::atomic<std::size_t> g_flushWarnings{0};
}
void setFlushFaultsForTesting(std::size_t count) noexcept
{
    g_flushFaults.store(count, std::memory_order_relaxed);
}
bool consumeFlushFaultForTesting() noexcept
{
    std::size_t current = g_flushFaults.load(std::memory_order_relaxed);
    while (current != 0) {
        if (g_flushFaults.compare_exchange_weak(current, current - 1,
                                                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}
std::size_t flushWarningCountForTesting() noexcept
{
    return g_flushWarnings.load(std::memory_order_relaxed);
}
void countFlushWarningForTesting() noexcept
{
    g_flushWarnings.fetch_add(1, std::memory_order_relaxed);
}
#endif // AR_ENABLE_TEST_HOOKS
} // namespace detail

namespace {

constexpr int kSchemaVersion = 1;
constexpr std::size_t kMaxAggregatedDiagnostics = 16;
constexpr std::size_t kTruncatedMessageBytes = 512;

using Json = nlohmann::json;

std::string timestampNow()
{
    const auto now = std::chrono::system_clock::now();
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char buffer[32];
    std::snprintf(
        buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
        utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
        static_cast<int>(millis));
    return buffer;
}

std::string threadIdText()
{
    return std::to_string(static_cast<unsigned long long>(std::hash<std::thread::id>{}(
        std::this_thread::get_id())));
}

bool isRunIdValid(std::string_view runId)
{
    if (runId.empty() || runId.size() > 64) {
        return false;
    }
    for (const char ch : runId) {
        const bool isDigit = ch >= '0' && ch <= '9';
        const bool isUpper = ch >= 'A' && ch <= 'Z';
        const bool isLower = ch >= 'a' && ch <= 'z';
        if (!isDigit && !isUpper && !isLower && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

Error makeError(ErrorCategory category, ErrorStage stage, std::string message)
{
    return Error{category, stage, std::move(message), std::nullopt};
}

Json encodeError(const Error& error)
{
    Json encoded;
    encoded["category"] = toString(error.category);
    encoded["stage"] = toString(error.stage);
    encoded["message"] = error.message;
    if (error.origin.has_value()) {
        encoded["origin"] = {{"domain", error.origin->domain}, {"code", error.origin->code}};
    } else {
        encoded["origin"] = nullptr;
    }
    return encoded;
}

// Encodes an error for a budget-constrained summary: the stable identity of the error is kept
// while the free text is capped, so a truncated summary still explains what failed.
Json encodeErrorCapped(const Error& error, std::size_t messageLimit)
{
    if (error.message.size() <= messageLimit) {
        return encodeError(error);
    }
    Error capped = error;
    capped.message.resize(messageLimit);
    capped.message += "...";
    return encodeError(capped);
}

const char* resourceKindName(ResourceKind kind)
{
    switch (kind) {
    case ResourceKind::Instance:
        return "instance";
    case ResourceKind::DispatchState:
        return "dispatch_state";
    case ResourceKind::OsModule:
        return "os_module";
    }
    return "instance";
}

const char* resourceStateName(ResourceState state)
{
    switch (state) {
    case ResourceState::Retained:
        return "retained";
    case ResourceState::Unknown:
        return "unknown";
    }
    return "unknown";
}

Json encodeSource(const SourceStamp& source)
{
    Json encoded;
    encoded["revision"] = source.revision.has_value() ? Json(*source.revision) : Json(nullptr);
    if (source.dirty.has_value()) {
        encoded["dirty"] = *source.dirty;
    } else {
        encoded["dirty"] = "unknown";
    }
    return encoded;
}

} // namespace

bool SourceStamp::operator==(const SourceStamp& other) const
{
    return revision == other.revision && dirty == other.dirty;
}

bool ResidualResource::operator==(const ResidualResource& other) const
{
    return kind == other.kind && state == other.state;
}

bool RunSummaryIdentity::operator==(const RunSummaryIdentity& other) const
{
    return buildId == other.buildId && inputHash == other.inputHash &&
           configuration == other.configuration && source == other.source;
}

bool EndResult::operator==(const EndResult& other) const
{
    return result == other.result && primaryResult == other.primaryResult &&
           primaryError == other.primaryError && diagnosticErrors == other.diagnosticErrors &&
           cleanupErrors == other.cleanupErrors &&
           diagnosticsComplete == other.diagnosticsComplete &&
           resourcesReleased == other.resourcesReleased &&
           residualResources == other.residualResources;
}

const char* toString(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warn";
    case LogLevel::Error:
        return "error";
    case LogLevel::Critical:
        return "critical";
    }
    return "info";
}

struct DiagnosticSession::Impl {
    enum class State {
        Open,
        Finishing,
        Closed,
    };

    DiagnosticConfig config;
    RunSummaryIdentity identity;
    std::filesystem::path runDirectory;

    std::unique_ptr<spdlog::logger> textLogger;
    std::unique_ptr<spdlog::logger> jsonlLogger;

    std::uint64_t textBytes = 0;
    std::uint64_t jsonlBytes = 0;
    bool textStopped = false;
    bool jsonlStopped = false;
    bool quotaReported = false;
    bool flushReported = false;

    State state = State::Open;
    std::uint64_t sequence = 0;
    bool incomplete = false;
    std::optional<Error> stickyError;
    std::map<std::pair<ErrorStage, ErrorCategory>, std::pair<Error, std::size_t>> aggregated;
    std::optional<EndResult> cachedResult;

    void recordDiagnostic(const Error& error)
    {
        incomplete = true;
        auto& slot = aggregated[{error.stage, error.category}];
        if (slot.second == 0) {
            slot.first = error;
        }
        ++slot.second;
    }

    std::vector<Error> diagnosticErrors() const
    {
        std::vector<Error> errors;
        errors.reserve(aggregated.size());
        for (const auto& [key, value] : aggregated) {
            (void)key;
            Error error = value.first;
            if (value.second > 1) {
                error.message += " (repeated " + std::to_string(value.second) + " times)";
            }
            errors.push_back(std::move(error));
            if (errors.size() >= kMaxAggregatedDiagnostics) {
                break;
            }
        }
        return errors;
    }

    std::expected<void, Error> writeLine(
        spdlog::logger& logger,
        const std::string& line,
        std::uint64_t& used,
        std::uint64_t budget,
        bool& stopped,
        const char* sinkName)
    {
        if (stopped) {
            return std::unexpected(
                makeError(ErrorCategory::OutputFailed, ErrorStage::Output,
                          std::string("sink stopped after reaching its byte budget: ") + sinkName));
        }
        const std::uint64_t cost = static_cast<std::uint64_t>(line.size()) + 1;
        if (used + cost > budget) {
            stopped = true;
            if (!quotaReported) {
                quotaReported = true;
                std::fprintf(
                    stderr,
                    "ar_core: diagnostic budget reached for %s; further output is suppressed\n",
                    sinkName);
            }
            recordDiagnostic(makeError(
                ErrorCategory::OutputFailed, ErrorStage::Output,
                std::string("byte budget exhausted for sink: ") + sinkName));
            return std::unexpected(makeError(
                ErrorCategory::OutputFailed, ErrorStage::Output,
                std::string("byte budget exhausted for sink: ") + sinkName));
        }
        try {
            logger.log(spdlog::level::trace, "{}", line);
        } catch (const spdlog::spdlog_ex& ex) {
            const Error error = makeError(
                ErrorCategory::OutputFailed, ErrorStage::Output,
                std::string("write failed for sink ") + sinkName + ": " + ex.what());
            stickyError = error;
            recordDiagnostic(error);
            return std::unexpected(error);
        }
        used += cost;
        return {};
    }

    bool flushSinks()
    {
        bool ok = true;
        for (auto* logger : {textLogger.get(), jsonlLogger.get()}) {
            if (logger == nullptr) {
                continue;
            }
            try {
#if defined(AR_ENABLE_TEST_HOOKS)
                if (detail::consumeFlushFaultForTesting()) {
                    throw spdlog::spdlog_ex("injected flush failure");
                }
#endif
                logger->flush();
            } catch (const spdlog::spdlog_ex& ex) {
                if (!flushReported) {
                    flushReported = true;
#if defined(AR_ENABLE_TEST_HOOKS)
                    detail::countFlushWarningForTesting();
#endif
                    std::fprintf(stderr,
                                 "ar_core: flushing diagnostics failed for run '%s'; the run "
                                 "evidence may be incomplete\n",
                                 config.runId.c_str());
                }
                const Error error = makeError(ErrorCategory::OutputFailed, ErrorStage::Output,
                                              std::string("flush failed: ") + ex.what());
                stickyError = error;
                recordDiagnostic(error);
                ok = false;
            }
        }
        return ok;
    }
};

DiagnosticSession::DiagnosticSession(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl))
{
}

DiagnosticSession::~DiagnosticSession()
{
    if (m_impl == nullptr) {
        return;
    }
    if (m_impl->state != Impl::State::Closed) {
        std::fprintf(
            stderr,
            "ar_core: diagnostic session for run '%s' destroyed without an explicit finish; "
            "the run result is not recorded\n",
            m_impl->config.runId.c_str());
        try {
            m_impl->flushSinks();
        } catch (...) {
        }
    }
    m_impl->textLogger.reset();
    m_impl->jsonlLogger.reset();
}

std::expected<std::unique_ptr<DiagnosticSession>, Error> DiagnosticSession::create(
    DiagnosticConfig config,
    RunSummaryIdentity identity)
{
    auto invalid = [](std::string message) {
        return std::unexpected(
            makeError(ErrorCategory::InvalidArguments, ErrorStage::Validation, std::move(message)));
    };
    auto openFailed = [](std::string message, std::optional<ErrorOrigin> origin = std::nullopt) {
        return std::unexpected(Error{ErrorCategory::OutputFailed, ErrorStage::Output,
                                     std::move(message), std::move(origin)});
    };

    if (!isRunIdValid(config.runId)) {
        return invalid("run_id must be 1-64 characters of [A-Za-z0-9_-]");
    }
    if (config.outputRoot.empty()) {
        return invalid("diagnostic output root is empty");
    }
    const DiagnosticQuota& quota = config.quota;
    if (quota.textBytes == 0 || quota.jsonlBytes == 0 || quota.singleEventBytes == 0 ||
        quota.summaryBytes == 0) {
        return invalid("diagnostic quotas must be finite positive byte counts");
    }

    std::error_code ec;
    std::filesystem::path root = std::filesystem::absolute(config.outputRoot, ec);
    if (ec) {
        return invalid("diagnostic output root is not a usable path: " + ec.message());
    }
    if (!std::filesystem::exists(root, ec)) {
        std::filesystem::create_directories(root, ec);
        if (ec) {
            return openFailed(
                "cannot create diagnostic output root: " + ec.message(),
                ErrorOrigin{"filesystem", ec.value()});
        }
    }
    if (!std::filesystem::is_directory(root, ec)) {
        const int code = ec ? ec.value() : static_cast<int>(std::errc::not_a_directory);
        return openFailed("diagnostic output root is not a directory: " + root.string(),
                          ErrorOrigin{"filesystem", code});
    }

    const std::filesystem::path runDirectory = root / config.runId;
    if (std::filesystem::exists(runDirectory, ec)) {
        return invalid("diagnostic run directory already exists: " + runDirectory.string());
    }
    std::filesystem::create_directory(runDirectory, ec);
    if (ec) {
        return openFailed(
            "cannot create diagnostic run directory: " + ec.message(),
            ErrorOrigin{"filesystem", ec.value()});
    }

    auto impl = std::make_unique<Impl>();
    impl->config = std::move(config);
    impl->identity = std::move(identity);
    impl->runDirectory = runDirectory;

    try {
        auto textSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            (runDirectory / "diagnostics.log").string(), true);
        auto jsonlSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            (runDirectory / "events.jsonl").string(), true);
        impl->textLogger = std::make_unique<spdlog::logger>("ar_core_text", textSink);
        impl->jsonlLogger = std::make_unique<spdlog::logger>("ar_core_jsonl", jsonlSink);
        impl->textLogger->set_pattern("%v");
        impl->jsonlLogger->set_pattern("%v");
        impl->textLogger->set_level(spdlog::level::trace);
        impl->jsonlLogger->set_level(spdlog::level::trace);
    } catch (const spdlog::spdlog_ex& ex) {
        std::fprintf(stderr, "ar_core: failed to open diagnostic streams: %s\n", ex.what());
        return openFailed(std::string("failed to open diagnostic streams: ") + ex.what());
    }

    return std::unique_ptr<DiagnosticSession>(new DiagnosticSession(std::move(impl)));
}

bool DiagnosticSession::enabled(LogLevel level) const noexcept
{
    return m_impl != nullptr &&
           static_cast<int>(level) >= static_cast<int>(m_impl->config.minLevel);
}

std::expected<void, Error> DiagnosticSession::emit(const Event& event)
{
    if (m_impl == nullptr) {
        return std::unexpected(
            makeError(ErrorCategory::InternalError, ErrorStage::Output, "session is not valid"));
    }
    Impl& impl = *m_impl;
    if (impl.state != Impl::State::Open) {
        return std::unexpected(makeError(ErrorCategory::InvalidState, ErrorStage::Validation,
                                         "invalid state: session is not open"));
    }
    if (impl.stickyError.has_value()) {
        return std::unexpected(*impl.stickyError);
    }
    if (!enabled(event.level)) {
        return {};
    }

    const std::string timestamp = timestampNow();
    std::string textLine = timestamp;
    textLine += ' ';
    textLine += toString(event.level);
    textLine += ' ';
    textLine += event.module;
    textLine += ' ';
    textLine += event.eventId;
    textLine += ' ';
    textLine += event.message;
    if (event.origin.has_value()) {
        textLine += " [";
        textLine += event.origin->domain;
        textLine += ':';
        textLine += std::to_string(event.origin->code);
        textLine += ']';
    }
    if (textLine.size() + 1 > impl.config.quota.singleEventBytes) {
        impl.incomplete = true;
        return std::unexpected(makeError(ErrorCategory::OutputFailed, ErrorStage::Output,
                                         "event exceeds the single-event byte limit"));
    }

    Json record;
    record["schema_version"] = kSchemaVersion;
    record["run_id"] = impl.config.runId;
    record["sequence"] = impl.sequence + 1;
    record["level"] = toString(event.level);
    record["module"] = event.module;
    record["event_id"] = event.eventId;
    record["stage"] = event.stage.has_value() ? Json(toString(*event.stage)) : Json(nullptr);
    record["message"] = event.message;
    record["origin"] =
        event.origin.has_value()
            ? Json{{"domain", event.origin->domain}, {"code", event.origin->code}}
            : Json(nullptr);
    record["timestamp"] = timestamp;
    record["thread_id"] = threadIdText();
    const std::string jsonLine = record.dump();
    if (jsonLine.size() + 1 > impl.config.quota.singleEventBytes) {
        impl.incomplete = true;
        return std::unexpected(makeError(ErrorCategory::OutputFailed, ErrorStage::Output,
                                         "event exceeds the single-event byte limit"));
    }

    ++impl.sequence;
    auto textResult = impl.writeLine(*impl.textLogger, textLine, impl.textBytes,
                                     impl.config.quota.textBytes, impl.textStopped, "text");
    auto jsonlResult = impl.writeLine(*impl.jsonlLogger, jsonLine, impl.jsonlBytes,
                                      impl.config.quota.jsonlBytes, impl.jsonlStopped, "jsonl");
    if (!textResult.has_value()) {
        return textResult;
    }
    return jsonlResult;
}

EndResult DiagnosticSession::finish(const RunSummaryInput& input)
{
    if (m_impl == nullptr) {
        EndResult result;
        result.result = ResultKind::OutputFailed;
        result.primaryResult = ResultKind::OutputFailed;
        return result;
    }
    Impl& impl = *m_impl;
    if (impl.state == Impl::State::Closed && impl.cachedResult.has_value()) {
        return *impl.cachedResult;
    }
    impl.state = Impl::State::Finishing;

    EndResult result;
    result.primaryResult = input.primaryResult;
    result.primaryError = input.primaryError;
    result.cleanupErrors = input.cleanupErrors;
    result.resourcesReleased = input.resourcesReleased;
    result.residualResources = input.residualResources;

    bool outputFailed = impl.stickyError.has_value();
    auto failSummary = [&](std::string message, ErrorCategory category = ErrorCategory::OutputFailed) {
        impl.recordDiagnostic(
            makeError(category, ErrorStage::Summary, std::move(message)));
        outputFailed = true;
    };

    // Echoed identity and resource facts must agree with what the session owns.
    if (input.schemaVersion != kSchemaVersion) {
        failSummary("summary schema_version does not match the session", ErrorCategory::InternalError);
    }
    if (input.runId != impl.config.runId) {
        failSummary("summary run_id does not match the session", ErrorCategory::InternalError);
    }
    if (!(input.identity == impl.identity)) {
        failSummary(
            "summary build identity does not match the compiled identity",
            ErrorCategory::InternalError);
    }
    if (input.resourcesReleased && !input.residualResources.empty()) {
        failSummary(
            "summary reports released resources together with residual resources",
            ErrorCategory::InternalError);
    }

    std::optional<Json> capability;
    const bool hasCapability = input.capabilityJson.has_value();
    const bool hasEncodingError = input.capabilityEncodingError.has_value();
    if (hasCapability && hasEncodingError) {
        impl.recordDiagnostic(*input.capabilityEncodingError);
        failSummary(
            "capability_json and capability_encoding_error were both supplied",
            ErrorCategory::InternalError);
    } else if (hasEncodingError) {
        impl.recordDiagnostic(*input.capabilityEncodingError);
        if (input.capabilityEncodingError->stage != ErrorStage::Summary ||
            input.capabilityEncodingError->category != ErrorCategory::OutputFailed) {
            failSummary(
                "capability_encoding_error must use stage=summary and category=output_failed",
                ErrorCategory::InternalError);
        } else {
            outputFailed = true;
        }
    } else if (hasCapability) {
        const std::string& text = *input.capabilityJson;
        try {
            Json parsed = Json::parse(text);
            if (!parsed.is_object()) {
                failSummary("capability_json must be a JSON object");
            } else {
                capability = std::move(parsed);
            }
        } catch (const Json::exception& ex) {
            failSummary(std::string("capability_json is not valid JSON: ") + ex.what());
        }
    }

    if (!impl.flushSinks()) {
        outputFailed = true;
    }
    // A dropped or refused diagnostic line makes the run's evidence incomplete, so the run
    // itself must not be reported as a clean success.
    if (impl.incomplete) {
        outputFailed = true;
    }

    const bool cleanupFailed = !input.cleanupErrors.empty();
    if (outputFailed) {
        result.result = ResultKind::OutputFailed;
    } else if (input.primaryResult == ResultKind::Success && cleanupFailed) {
        result.result = ResultKind::InitializationFailed;
    } else {
        result.result = input.primaryResult;
    }

    result.diagnosticErrors = impl.diagnosticErrors();
    result.diagnosticsComplete = !impl.incomplete && !outputFailed;

    Json summary;
    summary["schema_version"] = kSchemaVersion;
    summary["run_id"] = impl.config.runId;
    summary["build_id"] = impl.identity.buildId;
    summary["input_hash"] = impl.identity.inputHash;
    summary["configuration"] = impl.identity.configuration;
    summary["source"] = encodeSource(impl.identity.source);
    summary["primary_result"] = toString(result.primaryResult);
    summary["primary_error"] = result.primaryError.has_value() ? encodeError(*result.primaryError)
                                                              : Json(nullptr);
    summary["cleanup_errors"] = Json::array();
    for (const Error& error : result.cleanupErrors) {
        summary["cleanup_errors"].push_back(encodeError(error));
    }
    summary["diagnostic_errors"] = Json::array();
    for (const Error& error : result.diagnosticErrors) {
        summary["diagnostic_errors"].push_back(encodeError(error));
    }
    summary["result"] = toString(result.result);
    summary["diagnostics_complete"] = result.diagnosticsComplete;
    summary["resources_released"] = result.resourcesReleased;
    summary["residual_resources"] = Json::array();
    for (const ResidualResource& resource : result.residualResources) {
        summary["residual_resources"].push_back(
            {{"kind", resourceKindName(resource.kind)}, {"state", resourceStateName(resource.state)}});
    }
    summary["capability"] = capability.has_value() ? *capability : Json(nullptr);

    const std::filesystem::path summaryPath = impl.runDirectory / "summary.json";
    std::ofstream stream(summaryPath, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        failSummary("cannot open summary.json for writing");
        result.result = ResultKind::OutputFailed;
        result.diagnosticsComplete = false;
        std::fprintf(stderr, "ar_core: cannot write summary.json for run '%s'\n",
                     impl.config.runId.c_str());
    } else {
        std::string encoded = summary.dump(2);
        bool truncated = false;
        if (encoded.size() > impl.config.quota.summaryBytes) {
            truncated = true;
            // Record the failure first so the written fallback also carries this diagnostic error.
            failSummary("summary exceeds the summary byte budget");
            result.result = ResultKind::OutputFailed;
            result.diagnosticsComplete = false;
            result.diagnosticErrors = impl.diagnosticErrors();
            Json fallback;
            fallback["schema_version"] = kSchemaVersion;
            fallback["run_id"] = impl.config.runId;
            fallback["build_id"] = impl.identity.buildId;
            fallback["input_hash"] = impl.identity.inputHash;
            fallback["configuration"] = impl.identity.configuration;
            fallback["source"] = encodeSource(impl.identity.source);
            fallback["primary_result"] = toString(result.primaryResult);
            fallback["primary_error"] = result.primaryError.has_value()
                                            ? encodeErrorCapped(*result.primaryError, kTruncatedMessageBytes)
                                            : Json(nullptr);
            fallback["cleanup_errors"] = Json::array();
            for (const Error& error : result.cleanupErrors) {
                fallback["cleanup_errors"].push_back(
                    encodeErrorCapped(error, kTruncatedMessageBytes));
            }
            fallback["diagnostic_errors"] = Json::array();
            for (const Error& error : result.diagnosticErrors) {
                fallback["diagnostic_errors"].push_back(
                    encodeErrorCapped(error, kTruncatedMessageBytes));
            }
            fallback["result"] = toString(ResultKind::OutputFailed);
            fallback["diagnostics_complete"] = false;
            fallback["resources_released"] = result.resourcesReleased;
            fallback["residual_resources"] = summary["residual_resources"];
            fallback["capability"] = nullptr;
            fallback["summary_truncated"] = true;
            encoded = fallback.dump(2);
            // The capped fallback can still exceed a deliberately tiny budget; drop the error
            // lists but keep their counts and the primary error rather than losing everything.
            if (encoded.size() > impl.config.quota.summaryBytes) {
                fallback.erase("cleanup_errors");
                fallback.erase("diagnostic_errors");
                fallback["cleanup_error_count"] = static_cast<std::uint64_t>(
                    result.cleanupErrors.size());
                fallback["diagnostic_error_count"] = static_cast<std::uint64_t>(
                    result.diagnosticErrors.size());
                fallback["errors_omitted"] = true;
                encoded = fallback.dump(2);
            }
        }
        stream.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        stream.put('\n');
        stream.flush();
        if (!stream.good()) {
            failSummary("writing summary.json failed");
            std::fprintf(stderr, "ar_core: writing summary.json failed for run '%s'\n",
                         impl.config.runId.c_str());
        }
        stream.close();
        if (truncated) {
            result.result = ResultKind::OutputFailed;
            result.diagnosticsComplete = false;
        }
    }

    if (outputFailed) {
        result.result = ResultKind::OutputFailed;
        result.diagnosticsComplete = false;
        result.diagnosticErrors = impl.diagnosticErrors();
    }

    impl.state = Impl::State::Closed;
    impl.cachedResult = result;
    impl.textLogger.reset();
    impl.jsonlLogger.reset();
    return result;
}

} // namespace ar::core
