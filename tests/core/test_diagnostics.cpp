#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/BuildInfo.h"
#include "core/Diagnostics.h"
#include "core/Error.h"

#include "DiagnosticsTestHooks.h"

#if !defined(_MSVC_LANG)
#error "AR: _MSVC_LANG is required; the P0 target must be built with MSVC."
#endif

// Guards the test binary itself: a silent downgrade to C++14 must fail the build, not the run.
static_assert(_MSVC_LANG >= 202302L, "AR: tests must be compiled with /std:c++23preview.");

namespace {

namespace fs = std::filesystem;
using ar::core::DiagnosticConfig;
using ar::core::DiagnosticQuota;
using ar::core::DiagnosticSession;
using ar::core::EndResult;
using ar::core::Error;
using ar::core::ErrorCategory;
using ar::core::ErrorOrigin;
using ar::core::ErrorStage;
using ar::core::Event;
using ar::core::LogLevel;
using ar::core::ResidualResource;
using ar::core::ResourceKind;
using ar::core::ResourceState;
using ar::core::ResultKind;
using ar::core::RunSummaryIdentity;
using ar::core::RunSummaryInput;
using ar::core::SourceStamp;

fs::path freshRoot(std::string_view caseName)
{
#if defined(_MSC_VER)
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, "AR_TEST_OUTPUT_ROOT") != 0 || raw == nullptr) {
        throw std::runtime_error("AR_TEST_OUTPUT_ROOT is not set for the test run");
    }
    const std::string base(raw);
    std::free(raw);
#else
    const char* env = std::getenv("AR_TEST_OUTPUT_ROOT");
    if (env == nullptr) {
        throw std::runtime_error("AR_TEST_OUTPUT_ROOT is not set for the test run");
    }
    const std::string base(env);
#endif
    const fs::path root = fs::path(base) / std::string(caseName);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (ec) {
        throw std::runtime_error("cannot prepare test output root: " + ec.message());
    }
    return root;
}

std::string readAll(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return data;
}

std::vector<std::string> readLines(const fs::path& path)
{
    std::vector<std::string> lines;
    std::ifstream stream(path, std::ios::binary);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

RunSummaryIdentity testIdentity(std::string configuration = "Debug")
{
    return RunSummaryIdentity{
        "build-deadbeef",
        "input-0123456789",
        std::move(configuration),
        SourceStamp{std::string("6db7e5d"), std::optional<bool>(true)},
    };
}

DiagnosticConfig testConfig(const fs::path& root, std::string runId, LogLevel level = LogLevel::Debug)
{
    DiagnosticConfig config;
    config.outputRoot = root;
    config.runId = std::move(runId);
    config.minLevel = level;
    return config;
}

Event testEvent(LogLevel level, std::string eventId, std::string message)
{
    Event event;
    event.level = level;
    event.module = "core";
    event.eventId = std::move(eventId);
    event.message = std::move(message);
    return event;
}

nlohmann::json parseJson(const std::string& text)
{
    return nlohmann::json::parse(text);
}

bool hasDiagnosticError(const EndResult& result, ErrorStage stage, ErrorCategory category)
{
    return std::any_of(
        result.diagnosticErrors.begin(), result.diagnosticErrors.end(),
        [stage, category](const Error& error) {
            return error.stage == stage && error.category == category;
        });
}

} // namespace

TEST_CASE("a normal run writes both sinks and a complete summary", "[core-normal-close]")
{
    const fs::path root = freshRoot("core-normal-close");
    const RunSummaryIdentity identity = testIdentity();

    std::unique_ptr<DiagnosticSession> session;
    {
        auto created = DiagnosticSession::create(testConfig(root, "run-1"), identity);
        REQUIRE(created.has_value());
        session = std::move(*created);
    }

    CHECK(session->enabled(LogLevel::Info));
    CHECK(session->enabled(LogLevel::Debug));
    CHECK_FALSE(session->enabled(LogLevel::Trace));

    CHECK(session->emit(testEvent(LogLevel::Info, "startup", "engine starting")).has_value());
    Event warning = testEvent(LogLevel::Warning, "device", "adapter selected");
    warning.stage = ErrorStage::Capability;
    warning.origin = ErrorOrigin{"vulkan", -3};
    CHECK(session->emit(warning).has_value());
    CHECK(session->emit(testEvent(LogLevel::Error, "query", "capability queried")).has_value());

    RunSummaryInput input;
    input.runId = "run-1";
    input.identity = identity;
    input.primaryResult = ResultKind::Success;
    input.capabilityJson = std::string(R"({"api":"1.3.289","gpu":"RTX 4060"})");
    const EndResult result = session->finish(input);

    CHECK(result.result == ResultKind::Success);
    CHECK(result.primaryResult == ResultKind::Success);
    CHECK(result.diagnosticsComplete);
    CHECK(result.diagnosticErrors.empty());
    CHECK(result.cleanupErrors.empty());
    CHECK(result.resourcesReleased);

    const fs::path runDirectory = root / "run-1";
    const auto textLines = readLines(runDirectory / "diagnostics.log");
    REQUIRE(textLines.size() == 3);
    CHECK(textLines[0].find("info") != std::string::npos);
    CHECK(textLines[1].find("warn") != std::string::npos);
    CHECK(textLines[2].find("error") != std::string::npos);

    const auto jsonLines = readLines(runDirectory / "events.jsonl");
    REQUIRE(jsonLines.size() == 3);
    for (std::size_t index = 0; index < jsonLines.size(); ++index) {
        const nlohmann::json event = parseJson(jsonLines[index]);
        CHECK(event["schema_version"] == 1);
        CHECK(event["run_id"] == "run-1");
        CHECK(event["sequence"] == index + 1);
        CHECK(event["module"] == "core");
    }
    CHECK(parseJson(jsonLines[1])["origin"]["domain"] == "vulkan");
    CHECK(parseJson(jsonLines[1])["origin"]["code"] == -3);

    const nlohmann::json summary = parseJson(readAll(runDirectory / "summary.json"));
    CHECK(summary["schema_version"] == 1);
    CHECK(summary["run_id"] == "run-1");
    CHECK(summary["build_id"] == identity.buildId);
    CHECK(summary["input_hash"] == identity.inputHash);
    CHECK(summary["configuration"] == "Debug");
    CHECK(summary["source"]["revision"] == "6db7e5d");
    CHECK(summary["result"] == "success");
    CHECK(summary["diagnostics_complete"] == true);
    CHECK(summary["capability"]["api"] == "1.3.289");
    CHECK_FALSE(summary.contains("capability_json"));

    // The run directory is exclusive to one run.
    auto reused = DiagnosticSession::create(testConfig(root, "run-1"), identity);
    REQUIRE_FALSE(reused.has_value());
    CHECK(reused.error().category == ErrorCategory::InvalidArguments);
    CHECK(reused.error().stage == ErrorStage::Validation);
    CHECK_FALSE(fs::exists(root / "run-1" / "summary.json.tmp"));
}

TEST_CASE("an unusable output root fails before any run artifact exists", "[core-open-failed]")
{
    const fs::path root = freshRoot("core-open-failed");
    const fs::path blocker = root / "not-a-directory.txt";
    {
        std::ofstream stream(blocker, std::ios::binary);
        stream << "occupied\n";
    }

    const auto created = DiagnosticSession::create(testConfig(blocker, "run-1"), testIdentity());
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().category == ErrorCategory::OutputFailed);
    CHECK(created.error().stage == ErrorStage::Output);
    CHECK(created.error().origin.has_value());
    CHECK(created.error().origin->domain == "filesystem");

    // No session existed, so no run directory and no summary may have been written.
    CHECK_FALSE(fs::exists(blocker / "run-1"));
    CHECK_FALSE(fs::exists(blocker / "summary.json"));
}

TEST_CASE("event text is JSON-escaped and survives a round trip", "[core-json-escaping]")
{
    const fs::path root = freshRoot("core-json-escaping");
    const RunSummaryIdentity identity = testIdentity();
    auto created = DiagnosticSession::create(testConfig(root, "run-escape"), identity);
    REQUIRE(created.has_value());
    auto session = std::move(*created);

    const std::string message =
        "quote \" backslash \\ newline \n tab \t cr \r 中文测试 control \x01 end";
    CHECK(session->emit(testEvent(LogLevel::Info, "escaping", message)).has_value());

    RunSummaryInput input;
    input.runId = "run-escape";
    input.identity = identity;
    input.capabilityJson = std::string(R"({"名称":"显卡","值":1.5})");
    const EndResult result = session->finish(input);
    REQUIRE(result.result == ResultKind::Success);

    const auto jsonLines = readLines(root / "run-escape" / "events.jsonl");
    REQUIRE(jsonLines.size() == 1);
    const nlohmann::json event = parseJson(jsonLines[0]);
    CHECK(event["message"].get<std::string>() == message);

    const nlohmann::json summary = parseJson(readAll(root / "run-escape" / "summary.json"));
    CHECK(summary["capability"]["名称"] == "显卡");
}

TEST_CASE("quota exhaustion stops a sink and fails the run", "[core-quota]")
{
    const fs::path root = freshRoot("core-quota");
    const RunSummaryIdentity identity = testIdentity();

    SECTION("single event larger than the per-event limit")
    {
        DiagnosticConfig config = testConfig(root, "run-oversize");
        config.quota.singleEventBytes = 64;
        auto created = DiagnosticSession::create(config, identity);
        REQUIRE(created.has_value());
        auto session = std::move(*created);

        const auto emitted =
            session->emit(testEvent(LogLevel::Info, "big", std::string(200, 'x')));
        REQUIRE_FALSE(emitted.has_value());
        CHECK(emitted.error().category == ErrorCategory::OutputFailed);
        CHECK(emitted.error().stage == ErrorStage::Output);

        // A truncated record is worse than none: nothing may be written.
        CHECK_FALSE(fs::exists(root / "run-oversize" / "events.jsonl.tmp"));

        RunSummaryInput input;
        input.runId = "run-oversize";
        input.identity = identity;
        const EndResult result = session->finish(input);
        CHECK(result.result == ResultKind::OutputFailed);
        CHECK_FALSE(result.diagnosticsComplete);
    }

    SECTION("file budget")
    {
        DiagnosticConfig config = testConfig(root, "run-budget");
        config.quota.textBytes = 300;
        config.quota.jsonlBytes = 300;
        auto created = DiagnosticSession::create(config, identity);
        REQUIRE(created.has_value());
        auto session = std::move(*created);

        bool failed = false;
        for (int index = 0; index < 8 && !failed; ++index) {
            const auto emitted = session->emit(testEvent(
                LogLevel::Info, "fill", std::string(100, 'y')));
            failed = !emitted.has_value();
        }
        CHECK(failed);

        const fs::path runDirectory = root / "run-budget";
        CHECK(fs::file_size(runDirectory / "diagnostics.log") <= 300);
        CHECK(fs::file_size(runDirectory / "events.jsonl") <= 300);
        for (const std::string& line : readLines(runDirectory / "events.jsonl")) {
            CHECK_NOTHROW(parseJson(line));
        }

        RunSummaryInput input;
        input.runId = "run-budget";
        input.identity = identity;
        const EndResult result = session->finish(input);
        CHECK(result.result == ResultKind::OutputFailed);
        CHECK_FALSE(result.diagnosticsComplete);
        CHECK(hasDiagnosticError(result, ErrorStage::Output, ErrorCategory::OutputFailed));
    }
}

TEST_CASE("many small events are not mistaken for one oversized event", "[core-quota-cumulative]")
{
    const fs::path root = freshRoot("core-quota-cumulative");
    const RunSummaryIdentity identity = testIdentity();

    DiagnosticConfig config = testConfig(root, "run-cumulative");
    config.quota.singleEventBytes = 4096;
    config.quota.textBytes = 1U << 20;
    config.quota.jsonlBytes = 1U << 20;
    auto created = DiagnosticSession::create(config, identity);
    REQUIRE(created.has_value());
    auto session = std::move(*created);

    // Forty 200-byte messages are each well under the per-event limit, but together they far
    // exceed it; only the per-event size may be compared against singleEventBytes.
    for (int index = 0; index < 40; ++index) {
        const auto emitted =
            session->emit(testEvent(LogLevel::Info, "small", std::string(200, 'z')));
        REQUIRE(emitted.has_value());
    }

    RunSummaryInput input;
    input.runId = "run-cumulative";
    input.identity = identity;
    const EndResult result = session->finish(input);
    CHECK(result.result == ResultKind::Success);
    CHECK(result.diagnosticsComplete);
    CHECK(result.diagnosticErrors.empty());

    const fs::path runDirectory = root / "run-cumulative";
    CHECK(fs::file_size(runDirectory / "diagnostics.log") > config.quota.singleEventBytes);
    CHECK(readLines(runDirectory / "events.jsonl").size() == 40);
}

#if defined(AR_ENABLE_TEST_HOOKS)
TEST_CASE("a failing flush fails the run and keeps the primary error", "[core-flush-failure]")
{
    const fs::path root = freshRoot("core-flush-failure");
    const RunSummaryIdentity identity = testIdentity();

    DiagnosticConfig config = testConfig(root, "run-flush");
    auto created = DiagnosticSession::create(config, identity);
    REQUIRE(created.has_value());
    auto session = std::move(*created);

    CHECK(session->emit(testEvent(LogLevel::Info, "before", "before the flush")).has_value());

    // Both sinks fail on their flush attempt.
    ar::core::detail::setFlushFaultsForTesting(2);
    const std::size_t warningsBefore = ar::core::detail::flushWarningCountForTesting();

    RunSummaryInput input;
    input.runId = "run-flush";
    input.identity = identity;
    input.primaryResult = ResultKind::InitializationFailed;
    input.primaryError = Error{ErrorCategory::DeviceNotFound, ErrorStage::Enumeration,
                               "no suitable device", std::nullopt};
    const EndResult result = session->finish(input);

    CHECK(result.result == ResultKind::OutputFailed);
    CHECK_FALSE(result.diagnosticsComplete);
    REQUIRE(result.primaryError.has_value());
    CHECK(result.primaryError->category == ErrorCategory::DeviceNotFound);
    CHECK(hasDiagnosticError(result, ErrorStage::Output, ErrorCategory::OutputFailed));

    const nlohmann::json summary = parseJson(readAll(root / "run-flush" / "summary.json"));
    CHECK(summary["result"] == "output_failed");
    CHECK(summary["diagnostics_complete"] == false);
    CHECK(summary["primary_error"]["category"] == "device_not_found");

    // Two sinks failed, but the operator-facing warning is raised once per session.
    CHECK(ar::core::detail::flushWarningCountForTesting() == warningsBefore + 1);
}
#endif // AR_ENABLE_TEST_HOOKS

TEST_CASE("an oversized summary falls back without losing the primary error",
          "[core-summary-truncated]")
{
    const fs::path root = freshRoot("core-summary-truncated");
    const RunSummaryIdentity identity = testIdentity();

    DiagnosticConfig config = testConfig(root, "run-truncated");
    config.quota.summaryBytes = 600; // far smaller than any honest summary of this run
    auto created = DiagnosticSession::create(config, identity);
    REQUIRE(created.has_value());
    auto session = std::move(*created);

    RunSummaryInput input;
    input.runId = "run-truncated";
    input.identity = identity;
    input.primaryResult = ResultKind::InitializationFailed;
    input.primaryError =
        Error{ErrorCategory::DeviceNotFound, ErrorStage::Enumeration,
              std::string(400, 'p'), std::nullopt};
    for (int index = 0; index < 20; ++index) {
        input.cleanupErrors.push_back(
            Error{ErrorCategory::CleanupFailed, ErrorStage::Cleanup, std::string(400, 'c'),
                  ErrorOrigin{"os", index}});
    }
    const EndResult result = session->finish(input);

    CHECK(result.result == ResultKind::OutputFailed);
    CHECK_FALSE(result.diagnosticsComplete);
    REQUIRE(result.primaryError.has_value());
    CHECK(result.primaryError->category == ErrorCategory::DeviceNotFound);
    CHECK(hasDiagnosticError(result, ErrorStage::Summary, ErrorCategory::OutputFailed));

    const nlohmann::json summary = parseJson(readAll(root / "run-truncated" / "summary.json"));
    CHECK(summary["summary_truncated"] == true);
    CHECK(summary["result"] == "output_failed");
    CHECK(summary["diagnostics_complete"] == false);
    // The capped fallback still exceeds 600 bytes, so the arrays give way to counts while the
    // primary error and the identity block survive.
    CHECK(summary["errors_omitted"] == true);
    CHECK(summary["cleanup_error_count"] == 20);
    CHECK(summary["primary_error"]["category"] == "device_not_found");
    CHECK(summary["primary_error"]["message"].get<std::string>().size() <= 512);
}

TEST_CASE("finish is idempotent and writes the summary once", "[core-repeat-finish]")
{
    const fs::path root = freshRoot("core-repeat-finish");
    const RunSummaryIdentity identity = testIdentity();
    auto created = DiagnosticSession::create(testConfig(root, "run-twice"), identity);
    REQUIRE(created.has_value());
    auto session = std::move(*created);

    CHECK(session->emit(testEvent(LogLevel::Info, "once", "single event")).has_value());

    RunSummaryInput input;
    input.runId = "run-twice";
    input.identity = identity;
    const EndResult first = session->finish(input);
    const std::string summaryPath = (root / "run-twice" / "summary.json").string();
    const std::string firstSummary = readAll(summaryPath);
    const std::string firstLog = readAll(root / "run-twice" / "events.jsonl");

    const EndResult second = session->finish(input);
    CHECK(first == second);
    CHECK(readAll(summaryPath) == firstSummary);
    CHECK(readAll(root / "run-twice" / "events.jsonl") == firstLog);

    // Destruction after a successful finish must not write a second summary.
    session.reset();
    CHECK(readAll(summaryPath) == firstSummary);
}

TEST_CASE("events after finish are rejected without touching the artifacts", "[core-late-event]")
{
    static_assert(!std::is_copy_constructible_v<DiagnosticSession>);
    static_assert(!std::is_move_constructible_v<DiagnosticSession>);

    const fs::path root = freshRoot("core-late-event");
    const RunSummaryIdentity identity = testIdentity();
    auto created = DiagnosticSession::create(testConfig(root, "run-late"), identity);
    REQUIRE(created.has_value());
    auto session = std::move(*created);

    CHECK(session->emit(testEvent(LogLevel::Info, "before", "before finish")).has_value());

    RunSummaryInput input;
    input.runId = "run-late";
    input.identity = identity;
    const EndResult result = session->finish(input);
    REQUIRE(result.result == ResultKind::Success);

    const std::string logBefore = readAll(root / "run-late" / "diagnostics.log");
    const std::string summaryBefore = readAll(root / "run-late" / "summary.json");

    const auto late = session->emit(testEvent(LogLevel::Error, "after", "too late"));
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error().category == ErrorCategory::InvalidState);
    CHECK(late.error().stage == ErrorStage::Validation);
    CHECK(late.error().message.find("invalid state") != std::string::npos);

    CHECK(readAll(root / "run-late" / "diagnostics.log") == logBefore);
    CHECK(readAll(root / "run-late" / "summary.json") == summaryBefore);
}

TEST_CASE("an unwritable summary fails the run and keeps the primary error", "[report-unwritable]")
{
    const fs::path root = freshRoot("report-unwritable");
    const RunSummaryIdentity identity = testIdentity();
    auto created = DiagnosticSession::create(testConfig(root, "run-blocked"), identity);
    REQUIRE(created.has_value());
    auto session = std::move(*created);

    // Occupy the summary path with a directory so the stream cannot be opened.
    std::error_code ec;
    fs::create_directory(root / "run-blocked" / "summary.json", ec);
    REQUIRE_FALSE(ec);

    CHECK(session->emit(testEvent(LogLevel::Error, "cleanup", "device lost")).has_value());

    RunSummaryInput input;
    input.runId = "run-blocked";
    input.identity = identity;
    input.primaryResult = ResultKind::InitializationFailed;
    input.primaryError =
        Error{ErrorCategory::InitializationFailed, ErrorStage::Instance, "vkCreateInstance failed",
              ErrorOrigin{"vulkan", -9}};
    input.resourcesReleased = false;
    input.residualResources.push_back(
        ResidualResource{ResourceKind::Instance, ResourceState::Retained});

    const EndResult result = session->finish(input);
    CHECK(result.result == ResultKind::OutputFailed);
    CHECK_FALSE(result.diagnosticsComplete);
    REQUIRE(result.primaryError.has_value());
    CHECK(result.primaryError->category == ErrorCategory::InitializationFailed);
    CHECK(result.primaryError->origin->code == -9);
    CHECK(result.resourcesReleased == false);
    REQUIRE(result.residualResources.size() == 1);
    CHECK(hasDiagnosticError(result, ErrorStage::Summary, ErrorCategory::OutputFailed));
}
