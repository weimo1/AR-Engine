#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/BuildInfo.h"
#include "core/Error.h"

namespace ar::core {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

const char* toString(LogLevel level);

// Every bound is a finite positive byte budget. Zero or negative values are rejected.
struct DiagnosticQuota {
    std::uint64_t textBytes = 8ull * 1024ull * 1024ull;
    std::uint64_t jsonlBytes = 8ull * 1024ull * 1024ull;
    std::uint64_t singleEventBytes = 16ull * 1024ull;
    std::uint64_t summaryBytes = 64ull * 1024ull;
};

struct DiagnosticConfig {
    std::filesystem::path outputRoot;
    std::string runId;
    LogLevel minLevel = LogLevel::Info;
    DiagnosticQuota quota;
};

// Borrowed for the duration of emit; the session copies nothing and keeps no reference.
struct Event {
    LogLevel level = LogLevel::Info;
    std::string module;
    std::string eventId;
    std::string message;
    std::optional<ErrorStage> stage;
    std::optional<ErrorOrigin> origin;
};

enum class ResourceKind {
    Instance,
    DispatchState,
    OsModule,
    Presentation,
};

enum class ResourceState {
    Retained,
    Unknown,
};

struct ResidualResource {
    ResourceKind kind = ResourceKind::Instance;
    ResourceState state = ResourceState::Retained;

    bool operator==(const ResidualResource& other) const;
};

// Identity fixed at create time. finish compares the echoed values against it.
struct RunSummaryIdentity {
    std::string buildId;
    std::string inputHash;
    std::string configuration;
    SourceStamp source;

    bool operator==(const RunSummaryIdentity& other) const;
};

struct RunSummaryInput {
    int schemaVersion = 1;
    std::string runId;
    RunSummaryIdentity identity;
    ResultKind primaryResult = ResultKind::Success;
    std::optional<Error> primaryError;
    std::vector<Error> cleanupErrors;
    bool resourcesReleased = true;
    std::vector<ResidualResource> residualResources;
    // Already encoded UTF-8 JSON object text produced by the caller; nullopt when the run has
    // no capability product. core validates syntax and the top-level type only.
    std::optional<std::string> capabilityJson;
    std::optional<Error> capabilityEncodingError;
};

struct EndResult {
    ResultKind result = ResultKind::Success;
    ResultKind primaryResult = ResultKind::Success;
    std::optional<Error> primaryError;
    std::vector<Error> diagnosticErrors;
    std::vector<Error> cleanupErrors;
    bool diagnosticsComplete = true;
    bool resourcesReleased = true;
    std::vector<ResidualResource> residualResources;

    bool operator==(const EndResult& other) const;
};

// Single-owner, single-thread diagnostic session for one run.
class DiagnosticSession {
public:
    static std::expected<std::unique_ptr<DiagnosticSession>, Error>
    create(DiagnosticConfig config, RunSummaryIdentity identity);

    ~DiagnosticSession();

    DiagnosticSession(const DiagnosticSession&) = delete;
    DiagnosticSession& operator=(const DiagnosticSession&) = delete;
    DiagnosticSession(DiagnosticSession&&) = delete;
    DiagnosticSession& operator=(DiagnosticSession&&) = delete;

    // Read-only level filter: callers check before building an expensive message.
    [[nodiscard]] bool enabled(LogLevel level) const noexcept;

    // Encodes and writes both sinks before returning. After finish it returns
    // invalid_state with message "invalid state: ...", and nothing is written.
    std::expected<void, Error> emit(const Event& event);

    // Flushes the streams, writes summary.json once, and returns the owned end result.
    // Repeated calls return the cached result without writing again.
    EndResult finish(const RunSummaryInput& input);

private:
    struct Impl;

    explicit DiagnosticSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace ar::core
