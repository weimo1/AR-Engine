#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "TestingHooks.h"
#include "core/BuildInfo.h"
#include "core/Diagnostics.h"
#include "core/Error.h"
#include "rhi/Capabilities.h"
#include "rhi/RhiSession.h"
#include "runtime/RuntimeConfig.h"
#include "runtime/RuntimeSession.h"

namespace core = ar::core;
namespace rhi = ar::rhi;
namespace runtime = ar::runtime;

namespace {

using Json = nlohmann::json;

std::filesystem::path testOutputRoot() {
    char* buffer = nullptr;
    std::size_t size = 0;
    REQUIRE(_dupenv_s(&buffer, &size, "AR_TEST_OUTPUT_ROOT") == 0);
    REQUIRE(buffer != nullptr);
    std::unique_ptr<char, decltype(&std::free)> owned(buffer, &std::free);
    return std::filesystem::path(owned.get());
}

// Fresh exclusive run directory per case: the diagnostic session refuses an existing one.
std::filesystem::path freshRunDirectory(const std::string& runId) {
    const std::filesystem::path directory = testOutputRoot() / "runtime" / runId;
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory.parent_path());
    return directory;
}

std::filesystem::path fixtureModule() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    REQUIRE(length > 0);
    return std::filesystem::path(buffer).parent_path() / "ar_rhi_fixture.dll";
}

runtime::RuntimeConfig fixtureConfig(
    const std::string& runId,
    const char* spec,
    runtime::QueryMode mode = runtime::QueryMode::Inspect) {
    REQUIRE(_putenv_s("AR_RHI_FIXTURE_SPEC", spec) == 0);
    runtime::RuntimeConfig config;
    config.mode = mode;
    config.diagnostics.outputRoot = freshRunDirectory(runId);
    config.diagnostics.runId = runId;
    config.query.loader = rhi::ExplicitLoaderPath{fixtureModule()};
    config.query.requestedApi = rhi::kApiVersion1_4;
    return config;
}

Json readJson(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    Json value;
    input >> value;
    return value;
}

bool hasDiagnosticError(
    const core::EndResult& result, core::ErrorStage stage, core::ErrorCategory category) {
    for (const core::Error& error : result.diagnosticErrors) {
        if (error.stage == stage && error.category == category) {
            return true;
        }
    }
    return false;
}

// Runs one hidden case in a child process we own. A hung child is terminated through the exact
// handle we created, so a failing case can never leave a stray test process behind.
int runChildCase(const char* tag, DWORD timeoutMs = 120000) {
    wchar_t executable[MAX_PATH] = {};
    REQUIRE(::GetModuleFileNameW(nullptr, executable, MAX_PATH) > 0);

    std::wstring commandLine = L"\"";
    commandLine += executable;
    commandLine += L"\" \"";
    commandLine.append(tag, tag + std::strlen(tag));
    commandLine += L"\"";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    REQUIRE(
        ::CreateProcessW(
            nullptr,
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup,
            &process) != FALSE);

    const DWORD wait = ::WaitForSingleObject(process.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        ::TerminateProcess(process.hProcess, 97);
        ::WaitForSingleObject(process.hProcess, 10000);
    }
    DWORD exitCode = 1;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    REQUIRE(wait != WAIT_TIMEOUT);
    return static_cast<int>(exitCode);
}

} // namespace

TEST_CASE("a failed start rolls every acquired resource back in order", "[runtime-rollback]") {
    SECTION("an absent loader never creates an instance") {
        runtime::RuntimeConfig config = fixtureConfig("runtime-rollback-absent", "devices=1");
        config.query.loader = rhi::ExplicitLoaderPath{
            freshRunDirectory("runtime-rollback-absent") / "absent-loader.dll"};
        rhi::detail::resetProcessCounters();

        auto started = runtime::RuntimeSession::start(config);
        REQUIRE_FALSE(started.has_value());
        const runtime::StartFailure& failure = started.error();
        CHECK(failure.endResult.result == core::ResultKind::InitializationFailed);
        CHECK(failure.endResult.primaryError.has_value());
        CHECK(failure.endResult.primaryError->category == core::ErrorCategory::LoaderUnavailable);
        CHECK(failure.diagnosticsAvailable);
        CHECK(failure.endResult.resourcesReleased);
        CHECK(failure.endResult.cleanupErrors.empty());
        CHECK(failure.endResult.residualResources.empty());
        const rhi::detail::ProcessCounters counters = rhi::detail::processCounters();
        CHECK(counters.instanceCreateCalls == 0);
        CHECK(counters.destroyInstanceCalls == 0);
        CHECK(counters.volkInitializeCalls == 0);
        CHECK(
            ::GetFileAttributesW(
                (config.diagnostics.outputRoot / config.diagnostics.runId / "summary.json")
                    .c_str()) != INVALID_FILE_ATTRIBUTES);
    }

    SECTION("a missing entry point unloads the module it loaded") {
        runtime::RuntimeConfig config = fixtureConfig("runtime-rollback-symbol", "devices=1");
        wchar_t systemDirectory[MAX_PATH] = {};
        REQUIRE(::GetSystemDirectoryW(systemDirectory, MAX_PATH) > 0);
        config.query.loader =
            rhi::ExplicitLoaderPath{std::filesystem::path(systemDirectory) / "kernel32.dll"};
        rhi::detail::resetProcessCounters();

        auto started = runtime::RuntimeSession::start(config);
        REQUIRE_FALSE(started.has_value());
        CHECK(
            started.error().endResult.primaryError->category ==
            core::ErrorCategory::LoaderSymbolMissing);
        const rhi::detail::ProcessCounters counters = rhi::detail::processCounters();
        CHECK(counters.loadCalls == 1);
        CHECK(counters.volkInitializeCalls == 0);
        CHECK(counters.instanceCreateCalls == 0);
        CHECK(counters.destroyInstanceCalls == 0);
        CHECK(counters.unloadCalls == 1);
        CHECK(started.error().endResult.resourcesReleased);
    }

    SECTION("an instance creation failure has nothing left to destroy") {
        runtime::RuntimeConfig config =
            fixtureConfig("runtime-rollback-create", "create=init-failed");
        rhi::detail::resetProcessCounters();

        auto started = runtime::RuntimeSession::start(config);
        REQUIRE_FALSE(started.has_value());
        CHECK(
            started.error().endResult.primaryError->category ==
            core::ErrorCategory::InitializationFailed);
        const rhi::detail::ProcessCounters counters = rhi::detail::processCounters();
        CHECK(counters.loadCalls == 1);
        CHECK(counters.volkInitializeCalls == 1);
        CHECK(counters.instanceCreateCalls == 1);
        CHECK(counters.destroyInstanceCalls == 0);
        CHECK(counters.unloadCalls == 1);
        CHECK(started.error().endResult.cleanupErrors.empty());
        CHECK(started.error().endResult.residualResources.empty());
    }

    SECTION("an enumeration failure destroys the instance before unloading the module") {
        runtime::RuntimeConfig config = fixtureConfig("runtime-rollback-enumerate", "devices=0");
        rhi::detail::resetProcessCounters();

        auto started = runtime::RuntimeSession::start(config);
        REQUIRE_FALSE(started.has_value());
        CHECK(
            started.error().endResult.primaryError->category ==
            core::ErrorCategory::DeviceNotFound);
        const rhi::detail::ProcessCounters counters = rhi::detail::processCounters();
        CHECK(counters.instanceCreateCalls == 1);
        CHECK(counters.destroyInstanceCalls == 1);
        CHECK(counters.volkFinalizeCalls == 1);
        CHECK(counters.unloadCalls == 1);
        // Order, not just totals: the instance goes first, then the entry table, then the module.
        CHECK(rhi::detail::processShutdownOrder() == std::vector<int>{1, 2, 3});
        CHECK(started.error().endResult.resourcesReleased);
        CHECK(started.error().endResult.cleanupErrors.empty());
        CHECK(started.error().endResult.residualResources.empty());
    }
}

TEST_CASE("no physical device fails the start without switching devices", "[runtime-device-none]") {
    const std::string runId = "runtime-device-none";
    runtime::RuntimeConfig config = fixtureConfig(runId, "devices=0");

    auto started = runtime::RuntimeSession::start(config);
    REQUIRE_FALSE(started.has_value());
    const runtime::StartFailure& failure = started.error();
    CHECK(failure.endResult.result == core::ResultKind::InitializationFailed);
    CHECK(failure.endResult.primaryResult == core::ResultKind::InitializationFailed);
    CHECK(failure.endResult.primaryError->category == core::ErrorCategory::DeviceNotFound);
    CHECK(failure.endResult.primaryError->stage == core::ErrorStage::Enumeration);
    CHECK_FALSE(failure.report.deviceApi.has_value());
    CHECK_FALSE(failure.report.productTargetSupported);
    CHECK(failure.report.deviceName.empty());
    CHECK(failure.endResult.diagnosticsComplete);

    const Json summary = readJson(config.diagnostics.outputRoot / runId / "summary.json");
    CHECK(summary["result"] == "initialization_failed");
    CHECK(summary["primary_error"]["category"] == "device_not_found");
    CHECK(summary["capability"].is_null());
}

TEST_CASE("an out-of-range device index is invalid_arguments", "[runtime-device-index-invalid]") {
    const std::string runId = "runtime-device-index-invalid";
    runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1");
    config.query.device = rhi::ExplicitDeviceIndex{1};

    auto started = runtime::RuntimeSession::start(config);
    REQUIRE_FALSE(started.has_value());
    const runtime::StartFailure& failure = started.error();
    CHECK(failure.endResult.result == core::ResultKind::InvalidArguments);
    CHECK(failure.endResult.primaryError->category == core::ErrorCategory::InvalidArguments);
    CHECK(failure.endResult.primaryError->stage == core::ErrorStage::Enumeration);
    // No runtime session and no device was selected; the instance used for enumeration is
    // destroyed during rollback, which the rollback case asserts precisely.
    CHECK(failure.endResult.residualResources.empty());
    CHECK(failure.endResult.resourcesReleased);

    const Json summary = readJson(config.diagnostics.outputRoot / runId / "summary.json");
    CHECK(summary["result"] == "invalid_arguments");
}

TEST_CASE(
    "an unavailable destroy entry retains the instance in its own process",
    "[.][runtime-cleanup-destroy-child]") {
    {
        const std::string runId = "runtime-cleanup-failure-destroy";
        runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1;destroy=missing");
        auto started = runtime::RuntimeSession::start(config);
        REQUIRE(started.has_value());
        runtime::RuntimeSession& session = **started;
        const rhi::CapabilityReport before = session.report();
        const rhi::detail::ProcessCounters beforeClose = rhi::detail::processCounters();

        const core::EndResult first = session.close();
        CHECK(first.primaryResult == core::ResultKind::Success);
        CHECK(first.result == core::ResultKind::InitializationFailed);
        CHECK_FALSE(first.primaryError.has_value());
        REQUIRE(first.cleanupErrors.size() == 1);
        CHECK(first.cleanupErrors.front().stage == core::ErrorStage::Cleanup);
        CHECK(first.cleanupErrors.front().category == core::ErrorCategory::CleanupFailed);
        CHECK_FALSE(first.resourcesReleased);
        REQUIRE(first.residualResources.size() == 1);
        CHECK(first.residualResources.front().kind == core::ResourceKind::Instance);
        // The instance could not be released, so the loader module and the volk entry table stay
        // loaded: unloading them under a live instance would leave a dangling entry table. The
        // counters are process-wide, so this compares deltas around close().
        const rhi::detail::ProcessCounters counters = rhi::detail::processCounters();
        CHECK(counters.destroyInstanceCalls == beforeClose.destroyInstanceCalls + 1);
        CHECK(counters.volkFinalizeCalls == beforeClose.volkFinalizeCalls);
        CHECK(counters.unloadCalls == beforeClose.unloadCalls);

        const core::EndResult second = session.close();
        CHECK(second.result == first.result);
        CHECK(second.cleanupErrors.size() == first.cleanupErrors.size());
        CHECK(session.report().deviceName == before.deviceName);
        // The retained instance quarantines the process for later opens; this case only asserts
        // one cleanup, so the flag is cleared before the process exits.
        CHECK(rhi::detail::isProcessQuarantined());
        // The destructor is a fallback only: it must not clean up a second time.
        const rhi::detail::ProcessCounters beforeDestroy = rhi::detail::processCounters();
        started->reset();
        const rhi::detail::ProcessCounters afterDestroy = rhi::detail::processCounters();
        CHECK(afterDestroy.destroyInstanceCalls == beforeDestroy.destroyInstanceCalls);
        CHECK(afterDestroy.volkFinalizeCalls == beforeDestroy.volkFinalizeCalls);
        CHECK(afterDestroy.unloadCalls == beforeDestroy.unloadCalls);
    }
}

TEST_CASE(
    "an unload failure retains the OS module in its own process",
    "[.][runtime-cleanup-unload-child]") {
    {
        const std::string runId = "runtime-cleanup-failure-unload";
        runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1");
        auto started = runtime::RuntimeSession::start(config);
        REQUIRE(started.has_value());
        runtime::RuntimeSession& session = **started;

        rhi::detail::failNextUnloadForTesting();
        const core::EndResult first = session.close();
        CHECK(first.result == core::ResultKind::InitializationFailed);
        REQUIRE(first.cleanupErrors.size() == 1);
        CHECK(first.cleanupErrors.front().category == core::ErrorCategory::CleanupFailed);
        REQUIRE(first.residualResources.size() == 1);
        CHECK(first.residualResources.front().kind == core::ResourceKind::OsModule);
        // A retained module also quarantines the process, and the destructor must not retry.
        CHECK(rhi::detail::isProcessQuarantined());
        const rhi::detail::ProcessCounters beforeDestroy = rhi::detail::processCounters();
        started->reset();
        const rhi::detail::ProcessCounters afterDestroy = rhi::detail::processCounters();
        CHECK(afterDestroy.destroyInstanceCalls == beforeDestroy.destroyInstanceCalls);
        CHECK(afterDestroy.volkFinalizeCalls == beforeDestroy.volkFinalizeCalls);
        CHECK(afterDestroy.unloadCalls == beforeDestroy.unloadCalls);
    }
}

TEST_CASE(
    "a cleanup failure is recorded without replacing the business result",
    "[runtime-cleanup-failure]") {
    SECTION("an unavailable destroy entry retains the instance") {
        CHECK(runChildCase("[.][runtime-cleanup-destroy-child]") == 0);
    }
    SECTION("an unload failure retains the OS module after the instance is gone") {
        CHECK(runChildCase("[.][runtime-cleanup-unload-child]") == 0);
    }
}

TEST_CASE("a start without an instance has nothing to clean up", "[runtime-cleanup-empty]") {
    const std::string runId = "runtime-cleanup-empty";
    runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1");
    config.query.loader = rhi::ExplicitLoaderPath{freshRunDirectory(runId) / "absent-loader.dll"};

    auto started = runtime::RuntimeSession::start(config);
    REQUIRE_FALSE(started.has_value());
    const core::EndResult& result = started.error().endResult;
    CHECK(result.resourcesReleased);
    CHECK(result.residualResources.empty());
    CHECK(result.cleanupErrors.empty());
    CHECK(result.diagnosticsComplete);
    CHECK(result.primaryError->category == core::ErrorCategory::LoaderUnavailable);
}

TEST_CASE(
    "a quarantined process refuses a second open in a dedicated process",
    "[runtime-quarantine-reopen]") {
    CHECK(runChildCase("[.][runtime-quarantine-child]") == 0);
}

TEST_CASE(
    "repeated close returns the cached result and writes one summary", "[runtime-repeat-close]") {
    const std::string runId = "runtime-repeat-close";
    runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1");
    rhi::detail::resetProcessCounters();

    auto started = runtime::RuntimeSession::start(config);
    REQUIRE(started.has_value());
    runtime::RuntimeSession& session = **started;
    const rhi::CapabilityReport snapshot = session.report();

    const core::EndResult first = session.close();
    const Json summaryPath = readJson(config.diagnostics.outputRoot / runId / "summary.json");
    const auto summaryWriteTime =
        std::filesystem::last_write_time(config.diagnostics.outputRoot / runId / "summary.json");
    const rhi::detail::ProcessCounters afterFirst = rhi::detail::processCounters();

    const core::EndResult second = session.close();
    const core::EndResult third = session.close();
    CHECK(second.result == first.result);
    CHECK(third.result == first.result);
    CHECK(second.diagnosticsComplete == first.diagnosticsComplete);
    CHECK(second.cleanupErrors.size() == first.cleanupErrors.size());
    CHECK(
        std::filesystem::last_write_time(config.diagnostics.outputRoot / runId / "summary.json") ==
        summaryWriteTime);
    const rhi::detail::ProcessCounters afterThird = rhi::detail::processCounters();
    CHECK(afterThird.destroyInstanceCalls == afterFirst.destroyInstanceCalls);
    CHECK(afterThird.unloadCalls == afterFirst.unloadCalls);
    CHECK(session.report().deviceName == snapshot.deviceName);
    CHECK(summaryPath["run_id"] == runId);
    CHECK(summaryPath["result"] == "success");
}

TEST_CASE(
    "the summary embeds the runtime capability snapshot as an object",
    "[runtime-summary-capability]") {
    const std::string runId = "runtime-summary-capability";
    runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1");

    auto started = runtime::RuntimeSession::start(config);
    REQUIRE(started.has_value());
    runtime::RuntimeSession& session = **started;
    const core::EndResult result = session.close();
    CHECK(result.result == core::ResultKind::Success);
    CHECK(result.diagnosticsComplete);

    const Json summary = readJson(config.diagnostics.outputRoot / runId / "summary.json");
    REQUIRE(summary.contains("capability"));
    CHECK(summary["capability"].is_object());
    CHECK_FALSE(summary.contains("capability_json"));
    const Json& capability = summary["capability"];
    CHECK(capability["schema_version"] == 1);
    CHECK(capability["run_id"] == runId);
    CHECK(capability["mode"] == "inspect");
    CHECK(capability["build_id"] == core::buildInfo().buildId);
    CHECK(capability["capability_source"]["simulated"] == true);
    CHECK(capability["variant_supported"].is_string());
    // proposal D1: descriptor_heap is an object carrying a status, not a bare string.
    REQUIRE(capability["descriptor_heap"].is_object());
    CHECK(capability["descriptor_heap"]["status"].is_string());
    CHECK(capability["product_target_supported"].is_boolean());
    CHECK(capability["result"] == "success");
    CHECK(capability["errors"].is_array());
}

TEST_CASE(
    "a real system loader reports inspect facts without claiming 1.4", "[runtime-system-inspect]") {
    const std::string runId = "runtime-system-inspect";
    runtime::RuntimeConfig config;
    config.mode = runtime::QueryMode::Inspect;
    config.diagnostics.outputRoot = freshRunDirectory(runId);
    config.diagnostics.runId = runId;
    config.query.loader = rhi::SystemLoader{};
    config.query.requestedApi = rhi::kApiVersion1_4;

    auto started = runtime::RuntimeSession::start(config);
    REQUIRE(started.has_value());
    runtime::RuntimeSession& session = **started;
    const rhi::CapabilityReport& report = session.report();
    CHECK(report.capabilitySource.kind == "system_loader");
    CHECK_FALSE(report.capabilitySource.simulated);
    CHECK(report.capabilitySource.id.empty());
    CHECK(report.loaderApi.has_value());
    CHECK_FALSE(report.rendererRuntimeValidated);
    const bool productMet = report.productTargetSupported;
    CHECK(report.judgments.apiVersionSupported == productMet);

    const core::EndResult closed = session.close();
    CHECK(closed.result == core::ResultKind::Success);
}

TEST_CASE(
    "development mode queries an explicit 1.3 API without drawing",
    "[runtime-system-development]") {
    const std::string runId = "runtime-system-development";
    runtime::RuntimeConfig config;
    config.mode = runtime::QueryMode::Development;
    config.diagnostics.outputRoot = freshRunDirectory(runId);
    config.diagnostics.runId = runId;
    config.query.loader = rhi::SystemLoader{};
    config.query.requestedApi = rhi::makeApiVersion(1, 3);

    auto started = runtime::RuntimeSession::start(config);
    REQUIRE(started.has_value());
    runtime::RuntimeSession& session = **started;
    const rhi::CapabilityReport& report = session.report();
    // APIVariants carries the encoded variant number, and an explicit 1.3 request uses variant 0.
    CHECK(report.apiVariants.requested == 0);
    CHECK(report.instanceCreate.requested == rhi::makeApiVersion(1, 3));
    CHECK_FALSE(report.rendererRuntimeValidated);
    // The product target is 1.4, so an explicit 1.3 development query cannot satisfy it.
    CHECK_FALSE(report.productTargetSupported);

    const core::EndResult closed = session.close();
    CHECK(closed.result == core::ResultKind::Success);
    CHECK(closed.diagnosticsComplete);
}

TEST_CASE(
    "require_product rejects a complete query that misses the product target",
    "[runtime-require-product]") {
    const std::string runId = "runtime-require-product";
    runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1;device=1.3");
    config.mode = runtime::QueryMode::RequireProduct;
    config.query.requestedApi = rhi::kApiVersion1_4;

    auto started = runtime::RuntimeSession::start(config);
    REQUIRE_FALSE(started.has_value());
    const runtime::StartFailure& failure = started.error();
    // The query itself completed; only the 1.4 product verdict failed, so the result is
    // unsupported rather than an initialization failure.
    CHECK(failure.endResult.result == core::ResultKind::Unsupported);
    CHECK(failure.endResult.primaryResult == core::ResultKind::Unsupported);
    REQUIRE(failure.endResult.primaryError.has_value());
    CHECK(failure.endResult.primaryError->category == core::ErrorCategory::ApiVersionUnsupported);
    CHECK(failure.endResult.primaryError->stage == core::ErrorStage::Capability);
    CHECK_FALSE(failure.report.productTargetSupported);
    CHECK(failure.report.deviceApi.has_value());
    CHECK(failure.diagnosticsAvailable);
    CHECK(failure.endResult.diagnosticsComplete);

    const Json summary = readJson(config.diagnostics.outputRoot / runId / "summary.json");
    CHECK(summary["result"] == "unsupported");
    CHECK(summary["primary_error"]["category"] == "api_version_unsupported");
    REQUIRE(summary["capability"].is_object());
    CHECK(summary["capability"]["mode"] == "require_product");
    CHECK(summary["capability"]["product_target_supported"] == false);
}

TEST_CASE(
    "a requested API that contradicts the mode fails before loading Vulkan",
    "[runtime-mode-mismatch]") {
    struct Case {
        runtime::QueryMode mode;
        std::uint32_t requested;
        const char* runId;
    };
    const Case cases[] = {
        {runtime::QueryMode::Inspect, rhi::makeApiVersion(1, 2), "runtime-mode-mismatch-inspect"},
        {runtime::QueryMode::RequireProduct,
         rhi::makeApiVersion(1, 3),
         "runtime-mode-mismatch-require"},
        {runtime::QueryMode::Development,
         rhi::makeApiVersion(1, 4),
         "runtime-mode-mismatch-development"},
    };

    for (const Case& item : cases) {
        INFO("run " << item.runId);
        runtime::RuntimeConfig config = fixtureConfig(item.runId, "devices=1");
        config.mode = item.mode;
        config.query.requestedApi = item.requested;
        rhi::detail::resetProcessCounters();

        auto started = runtime::RuntimeSession::start(config);
        REQUIRE_FALSE(started.has_value());
        const runtime::StartFailure& failure = started.error();
        CHECK(failure.endResult.result == core::ResultKind::InvalidArguments);
        REQUIRE(failure.endResult.primaryError.has_value());
        CHECK(failure.endResult.primaryError->category == core::ErrorCategory::InvalidArguments);
        CHECK(failure.endResult.primaryError->stage == core::ErrorStage::Validation);
        // Rejected before the diagnostic session exists, so no run directory is created.
        CHECK_FALSE(failure.diagnosticsAvailable);
        CHECK_FALSE(failure.endResult.diagnosticsComplete);
        CHECK(failure.endResult.resourcesReleased);
        CHECK(failure.endResult.cleanupErrors.empty());
        // A rejected start must describe itself, not inherit the report's default success.
        CHECK(failure.report.result == core::ResultKind::InvalidArguments);
        CHECK_FALSE(
            std::filesystem::exists(config.diagnostics.outputRoot / item.runId / "summary.json"));

        const rhi::detail::ProcessCounters counters = rhi::detail::processCounters();
        CHECK(counters.loadCalls == 0);
        CHECK(counters.quarantineChecks == 0);
        CHECK(counters.instanceCreateCalls == 0);
    }
}

TEST_CASE(
    "a diagnostic output failure ends as output_failed without probing quarantine",
    "[runtime-diagnostic-create-failure]") {
    const std::string runId = "runtime-diagnostic-create-failure";
    runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1");
    // A regular file where the session expects its run directory: create() fails before the
    // diagnostic session exists, so the rhi layer must never run, not even the quarantine probe.
    const std::filesystem::path caseDirectory = freshRunDirectory(runId);
    std::filesystem::create_directories(caseDirectory);
    const std::filesystem::path blocker = caseDirectory / "blocker";
    {
        std::ofstream out(blocker);
        REQUIRE(out.good());
        out << "not a directory";
    }
    REQUIRE(std::filesystem::is_regular_file(blocker));
    config.diagnostics.outputRoot = blocker;
    rhi::detail::resetProcessCounters();
    rhi::detail::clearProcessQuarantine();

    auto started = runtime::RuntimeSession::start(config);
    REQUIRE_FALSE(started.has_value());
    const runtime::StartFailure& failure = started.error();
    CHECK(failure.endResult.result == core::ResultKind::OutputFailed);
    CHECK_FALSE(failure.endResult.diagnosticsComplete);
    CHECK(failure.endResult.resourcesReleased);
    CHECK_FALSE(failure.diagnosticsAvailable);
    CHECK(failure.report.result == core::ResultKind::OutputFailed);

    const rhi::detail::ProcessCounters counters = rhi::detail::processCounters();
    CHECK(counters.quarantineChecks == 0);
    CHECK(counters.loadCalls == 0);
    CHECK(counters.volkInitializeCalls == 0);
    CHECK(counters.instanceCreateCalls == 0);
    CHECK_FALSE(rhi::detail::isProcessQuarantined());
}

TEST_CASE(
    "a summary that cannot be written ends the run as output_failed",
    "[runtime-summary-output-failure]") {
    SECTION("an unwritable summary keeps the business result and the primary error slot empty") {
        const std::string runId = "runtime-summary-unwritable";
        runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1;destroy=missing");
        auto started = runtime::RuntimeSession::start(config);
        REQUIRE(started.has_value());
        runtime::RuntimeSession& session = **started;

        // The run directory was created by the session, so a directory in the summary slot makes
        // the write fail without disturbing anything the start already reported.
        const std::filesystem::path summaryPath =
            config.diagnostics.outputRoot / runId / "summary.json";
        std::error_code ec;
        std::filesystem::create_directory(summaryPath, ec);
        REQUIRE_FALSE(ec);

        const core::EndResult closed = session.close();
        CHECK(closed.result == core::ResultKind::OutputFailed);
        CHECK(closed.primaryResult == core::ResultKind::Success);
        CHECK_FALSE(closed.primaryError.has_value());
        CHECK_FALSE(closed.diagnosticsComplete);
        CHECK(hasDiagnosticError(
            closed, core::ErrorStage::Summary, core::ErrorCategory::OutputFailed));
        // The close-time cleanup failure is still reported as its own fact.
        REQUIRE(closed.cleanupErrors.size() == 1);
        CHECK(closed.cleanupErrors.front().category == core::ErrorCategory::CleanupFailed);
        CHECK(rhi::detail::isProcessQuarantined());
        rhi::detail::clearProcessQuarantine();
    }

    SECTION("an over-budget summary falls back and drops the capability payload") {
        const std::string runId = "runtime-summary-budget";
        runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1");
        config.diagnostics.quota.summaryBytes = 400;
        auto started = runtime::RuntimeSession::start(config);
        REQUIRE(started.has_value());
        runtime::RuntimeSession& session = **started;

        const core::EndResult closed = session.close();
        CHECK(closed.result == core::ResultKind::OutputFailed);
        CHECK(closed.primaryResult == core::ResultKind::Success);
        CHECK_FALSE(closed.diagnosticsComplete);

        const Json summary = readJson(config.diagnostics.outputRoot / runId / "summary.json");
        CHECK(summary["summary_truncated"] == true);
        CHECK(summary["capability"].is_null());
        CHECK(summary["result"] == "output_failed");
        CHECK(summary["primary_result"] == "success");
    }
}

TEST_CASE(
    "an unencodable capability fails the summary without touching the business result",
    "[runtime-capability-encode-failure]") {
    const std::string runId = "runtime-capability-encode-failure";
    // The fixture device name carries raw 0xC3 0x28, which is not valid UTF-8, so the capability
    // encoder hits a real serialization boundary instead of a stubbed fault.
    runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1;device-name-hex=C328");
    auto started = runtime::RuntimeSession::start(config);
    REQUIRE(started.has_value());
    runtime::RuntimeSession& session = **started;
    const rhi::CapabilityReport snapshot = session.report();

    const core::EndResult closed = session.close();
    CHECK(closed.result == core::ResultKind::OutputFailed);
    CHECK_FALSE(closed.primaryError.has_value());
    CHECK_FALSE(closed.diagnosticsComplete);
    CHECK(hasDiagnosticError(closed, core::ErrorStage::Summary, core::ErrorCategory::OutputFailed));

    const Json summary = readJson(config.diagnostics.outputRoot / runId / "summary.json");
    CHECK(summary["capability"].is_null());
    CHECK(summary["result"] == "output_failed");
    CHECK(summary["primary_result"] == "success");
    // The runtime snapshot itself is unaffected: only the encoded payload failed.
    CHECK(snapshot.deviceName == session.report().deviceName);
}

TEST_CASE(
    "the destructor releases the loader but never finishes the diagnostics",
    "[runtime-destructor]") {
    const std::string runId = "runtime-destructor";
    runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1");
    const rhi::detail::ProcessCounters beforeStart = rhi::detail::processCounters();

    auto started = runtime::RuntimeSession::start(config);
    REQUIRE(started.has_value());
    const rhi::detail::ProcessCounters beforeDestroy = rhi::detail::processCounters();
    CHECK(beforeDestroy.unloadCalls == beforeStart.unloadCalls);

    started->reset();

    // The fallback path releases the loader resources in the documented order but leaves the
    // diagnostics unfinished: no summary.json is invented after the fact.
    const rhi::detail::ProcessCounters afterDestroy = rhi::detail::processCounters();
    CHECK(afterDestroy.destroyInstanceCalls == beforeDestroy.destroyInstanceCalls + 1);
    CHECK(afterDestroy.volkFinalizeCalls == beforeDestroy.volkFinalizeCalls + 1);
    CHECK(afterDestroy.unloadCalls == beforeDestroy.unloadCalls + 1);
    CHECK_FALSE(std::filesystem::exists(config.diagnostics.outputRoot / runId / "summary.json"));
}

TEST_CASE(
    "a quarantined child asserts the exact runtime rejection", "[.][runtime-quarantine-child]") {
    const std::string runId = "runtime-quarantine-child";
    runtime::RuntimeConfig config = fixtureConfig(runId, "devices=1;destroy=missing");

    auto first = runtime::RuntimeSession::start(config);
    REQUIRE(first.has_value());
    const core::EndResult firstClose = (*first)->close();
    CHECK(firstClose.result == core::ResultKind::InitializationFailed);
    CHECK(rhi::detail::isProcessQuarantined());

    rhi::detail::resetProcessCounters();
    runtime::RuntimeConfig second = config;
    second.diagnostics.outputRoot = freshRunDirectory("runtime-quarantine-child-second");
    second.diagnostics.runId = "runtime-quarantine-child-second";
    auto reopened = runtime::RuntimeSession::start(second);
    REQUIRE_FALSE(reopened.has_value());
    const runtime::StartFailure& failure = reopened.error();
    CHECK(failure.endResult.result == core::ResultKind::InitializationFailed);
    REQUIRE(failure.endResult.primaryError.has_value());
    CHECK(failure.endResult.primaryError->category == core::ErrorCategory::CleanupFailed);
    CHECK(failure.endResult.primaryError->stage == core::ErrorStage::Cleanup);
    REQUIRE(failure.endResult.primaryError->origin.has_value());
    CHECK(failure.endResult.primaryError->origin->domain == "internal");
    CHECK(failure.endResult.primaryError->origin->code == 1);
    CHECK(failure.endResult.resourcesReleased);
    CHECK(failure.endResult.residualResources.empty());
    CHECK(failure.endResult.cleanupErrors.empty());
    CHECK(failure.report.instanceCreate.status == rhi::InstanceCreateStatus::NotAttempted);
    CHECK_FALSE(failure.report.loaderApi.has_value());
    CHECK_FALSE(failure.report.deviceApi.has_value());
    CHECK_FALSE(failure.report.productTargetSupported);

    const rhi::detail::ProcessCounters counters = rhi::detail::processCounters();
    CHECK(counters.loadCalls == 0);
    CHECK(counters.volkInitializeCalls == 0);
    CHECK(counters.instanceCreateCalls == 0);
    CHECK(rhi::detail::isProcessQuarantined());

    // A second consecutive open is refused exactly like the first: the quarantine is sticky.
    runtime::RuntimeConfig third = config;
    third.diagnostics.outputRoot = freshRunDirectory("runtime-quarantine-child-third");
    third.diagnostics.runId = "runtime-quarantine-child-third";
    auto again = runtime::RuntimeSession::start(third);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().endResult.result == core::ResultKind::InitializationFailed);
    REQUIRE(again.error().endResult.primaryError.has_value());
    CHECK(again.error().endResult.primaryError->category == core::ErrorCategory::CleanupFailed);
    CHECK(again.error().report.instanceCreate.status == rhi::InstanceCreateStatus::NotAttempted);
    CHECK(rhi::detail::processCounters().instanceCreateCalls == 0);

    // A diagnostic output failure in an already quarantined process ends as output_failed and
    // must not even reach the quarantine check, nor lift it.
    const std::filesystem::path blockedRoot = freshRunDirectory("runtime-quarantine-child-blocked");
    {
        std::ofstream blocker(blockedRoot);
        REQUIRE(blocker.good());
    }
    REQUIRE(std::filesystem::is_regular_file(blockedRoot));
    const rhi::detail::ProcessCounters beforeBlocked = rhi::detail::processCounters();
    runtime::RuntimeConfig fourth = config;
    fourth.diagnostics.outputRoot = blockedRoot;
    fourth.diagnostics.runId = "runtime-quarantine-child";
    auto blocked = runtime::RuntimeSession::start(fourth);
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error().endResult.result == core::ResultKind::OutputFailed);
    CHECK_FALSE(blocked.error().diagnosticsAvailable);
    CHECK(rhi::detail::processCounters().quarantineChecks == beforeBlocked.quarantineChecks);
    CHECK(rhi::detail::isProcessQuarantined());
}
