#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "TestingHooks.h"
#include "core/BuildInfo.h"
#include "core/Diagnostics.h"
#include "core/Error.h"
#include "platform/Window.h"
#include "render/EnvironmentLighting.h"
#include "render/PresentationGraph.h"
#include "render/PresentationSpec.h"
#include "rhi/Capabilities.h"
#include "rhi/RhiSession.h"
#include "runtime/RuntimeConfig.h"
#include "runtime/RuntimeSession.h"
#include "vulkan/binding/DescriptorHeapLayout.h"

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

// Shown only when the assertion after it fails, so a machine whose real loader refuses to start
// says why (missing vulkan-1.dll, no device, ...) instead of a bare "false".
std::string describeStart(
    const std::expected<std::unique_ptr<runtime::RuntimeSession>, runtime::StartFailure>& started) {
    if (started.has_value()) {
        return "started";
    }
    if (!started.error().endResult.primaryError.has_value()) {
        return "start failed without a primary error";
    }
    const core::Error& error = *started.error().endResult.primaryError;
    std::string text = std::string("start failed: ") + core::toString(error.category) + " at " +
                       core::toString(error.stage) + ": " + error.message;
    if (error.origin.has_value()) {
        text += " (" + error.origin->domain + " " + std::to_string(error.origin->code) + ")";
    }
    return text;
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
    INFO(describeStart(started));
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
    INFO(describeStart(started));
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

TEST_CASE("Hidden window rebuilds, pauses, restores and closes", "[sample-resize]") {
    runtime::RuntimeConfig config;
    config.mode = runtime::QueryMode::Development;
    config.query.requestedApi = rhi::makeApiVersion(1, 3);
    config.query.createGraphicsDevice = true;
    config.query.enableScreenshot = true;
    bool graph = false;
    SECTION("triangle") {}
    SECTION("GBuffer graph") {
        graph = true;
    }
    const auto shader = [](const char* name) {
        std::ifstream input(
            std::filesystem::path(AR_TEST_SHADER_DIR) / name, std::ios::binary | std::ios::ate);
        const auto size = static_cast<std::streamoff>(input.tellg());
        REQUIRE(size > 0);
        REQUIRE(size % 4 == 0);
        std::vector<std::uint32_t> words(static_cast<std::size_t>(size / 4));
        REQUIRE(input.seekg(0).read(reinterpret_cast<char*>(words.data()), size));
        return words;
    };
    config.query.triangleShaders = rhi::TriangleShaders{
        shader(graph ? "gbuffer.vert.spv" : "triangle.vert.spv"),
        shader(graph ? "gbuffer.frag.spv" : "triangle.frag.spv")};
    config.window.emplace();
    config.window->visible = false;
    config.window->title = "AR resize test " + std::to_string(GetCurrentProcessId());
    config.diagnostics.runId = "sample-resize";
    config.diagnostics.outputRoot = freshRunDirectory(config.diagnostics.runId);
    if (graph) {
        config.query.enableGBuffer = true;
        config.staticAssetRoot = config.diagnostics.outputRoot / "assets";
        std::filesystem::create_directories(*config.staticAssetRoot);
    }
    auto session = runtime::RuntimeSession::start(config);
    REQUIRE(session);
    const std::wstring title(config.window->title.begin(), config.window->title.end());
    const HWND window = FindWindowW(nullptr, title.c_str());
    REQUIRE(window != nullptr);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    REQUIRE(owner == GetCurrentProcessId());
    REQUIRE_FALSE(IsWindowVisible(window));

    const auto present = [&](rhi::FrameScreenshot* screenshot = nullptr) {
        auto result = (*session)->presentFrame(screenshot);
        REQUIRE(result);
        return *result;
    };
    REQUIRE(present() == runtime::FrameStatus::Presented);
    rhi::FrameScreenshot before;
    REQUIRE(present(&before) == runtime::FrameStatus::Presented);
    REQUIRE(before.width > 0);
    REQUIRE(before.height > 0);
    REQUIRE(
        SetWindowPos(window, nullptr, 0, 0, 840, 520, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    RECT client{};
    REQUIRE(GetClientRect(window, &client));
    rhi::FrameScreenshot after;
    REQUIRE(present(&after) == runtime::FrameStatus::Presented);
    CHECK(after.width == static_cast<std::uint32_t>(client.right));
    CHECK(after.height == static_cast<std::uint32_t>(client.bottom));
    CHECK(after.width != before.width);
    CHECK(after.height != before.height);
    REQUIRE(after.rgba.size() == static_cast<std::size_t>(after.width) * after.height * 4);
    const auto center =
        (static_cast<std::size_t>(after.height / 2) * after.width + after.width / 2) * 4;
    if (graph) {
        REQUIRE(after.gbuffer.size() == 3);
        for (const auto& probe : after.gbuffer) {
            CHECK(probe.depth == 0.F);
            CHECK(probe.pixel[0] < after.width);
            CHECK(probe.pixel[1] < after.height);
        }
        // Reuse the rebuilt slots with both capture modes.
        REQUIRE(present() == runtime::FrameStatus::Presented);
        REQUIRE(present(&after) == runtime::FrameStatus::Presented);
        REQUIRE(after.gbuffer.size() == 3);
    } else {
        CHECK(after.rgba[center] > after.rgba[0]);
    }

    // Change the iconic style directly so the test window remains hidden and never activates.
    const LONG_PTR normalStyle = GetWindowLongPtrW(window, GWL_STYLE);
    REQUIRE(SetWindowLongPtrW(window, GWL_STYLE, normalStyle | WS_MINIMIZE) != 0);
    REQUIRE(IsIconic(window));
    CHECK_FALSE(IsWindowVisible(window));
    CHECK(present() == runtime::FrameStatus::Paused);
    REQUIRE(SetWindowLongPtrW(window, GWL_STYLE, normalStyle) != 0);
    REQUIRE_FALSE(IsIconic(window));
    CHECK_FALSE(IsWindowVisible(window));
    REQUIRE(present() == runtime::FrameStatus::Presented);

    REQUIRE(PostMessageW(window, WM_CLOSE, 0, 0));
    REQUIRE(present() == runtime::FrameStatus::Closed);
    const auto closed = (*session)->close();
    REQUIRE(closed.result == core::ResultKind::Success);
    REQUIRE(closed.resourcesReleased);
    CHECK_FALSE(IsWindow(window));
    std::ifstream events(config.diagnostics.outputRoot / config.diagnostics.runId / "events.jsonl");
    REQUIRE(events);
    std::size_t starts = 0;
    for (std::string line; std::getline(events, line);) {
        const auto id = Json::parse(line).at("event_id").get<std::string>();
        CHECK(id != "rhi.validation.error");
        starts += id == "rhi.query.start";
    }
    CHECK(starts == 1);
}

TEST_CASE(
    "Camera view updates re-render without re-uploading static scene resources",
    "[sample-camera-view-update]") {
    bool indirect = false;
    std::string runSuffix;
    SECTION("indirect") {
        indirect = true;
        runSuffix = "indirect";
    }
    SECTION("direct") {
        indirect = false;
        runSuffix = "direct";
    }

    const auto shader = [](const char* name) {
        std::ifstream input(
            std::filesystem::path(AR_TEST_SHADER_DIR) / name, std::ios::binary | std::ios::ate);
        const auto size = static_cast<std::streamoff>(input.tellg());
        REQUIRE(size > 0);
        REQUIRE(size % 4 == 0);
        std::vector<std::uint32_t> words(static_cast<std::size_t>(size / 4));
        REQUIRE(input.seekg(0).read(reinterpret_cast<char*>(words.data()), size));
        return words;
    };

    const std::string runId = "sample-camera-view-update-" + runSuffix;
    const std::filesystem::path runRoot = freshRunDirectory(runId);
    const std::filesystem::path assetRoot = runRoot / "assets";
    const std::filesystem::path modelsDir = assetRoot / "models";
    std::filesystem::create_directories(modelsDir);

    // Deterministic [-1, 1] x [-1, 1] XY quad at z = 0 facing +Z with 2x1 red/blue PNG.
    const std::array<float, 12> positions{
        -1.F, -1.F, 0.F, 1.F, -1.F, 0.F, 1.F, 1.F, 0.F, -1.F, 1.F, 0.F};
    const std::array<float, 8> uvs{0.F, 0.F, 1.F, 0.F, 1.F, 1.F, 0.F, 1.F};
    const std::array<std::uint16_t, 6> indices{0, 1, 2, 0, 2, 3};
    const std::array<float, 12> normals{0.F, 0.F, 1.F, 0.F, 0.F, 1.F, 0.F, 0.F, 1.F, 0.F, 0.F, 1.F};
    constexpr std::array<unsigned char, 71> png{
        137, 80,  78, 71,  13,  10,  26,  10, 0,   0,   0,   13,  73, 72,  68,  82, 0,   0,
        0,   2,   0,  0,   0,   1,   8,   6,  0,   0,   0,   244, 34, 127, 138, 0,  0,   0,
        14,  73,  68, 65,  84,  120, 156, 99, 248, 207, 192, 0,   66, 255, 1,   15, 249, 3,
        253, 133, 17, 153, 118, 0,   0,   0,  0,   73,  69,  78,  68, 174, 66,  96, 130};
    {
        std::ofstream bin(modelsDir / "quad.bin", std::ios::binary);
        bin.write(
            reinterpret_cast<const char*>(positions.data()),
            static_cast<std::streamsize>(sizeof(positions)));
        bin.write(
            reinterpret_cast<const char*>(uvs.data()), static_cast<std::streamsize>(sizeof(uvs)));
        bin.write(
            reinterpret_cast<const char*>(indices.data()),
            static_cast<std::streamsize>(sizeof(indices)));
        bin.write(
            reinterpret_cast<const char*>(normals.data()),
            static_cast<std::streamsize>(sizeof(normals)));
        REQUIRE(bin.good());
    }
    {
        std::ofstream image(modelsDir / "colors.png", std::ios::binary);
        image.write(
            reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(sizeof(png)));
        REQUIRE(image.good());
    }
    {
        std::ofstream gltf(modelsDir / "quad.gltf", std::ios::binary);
        gltf << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
             << R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":)"
             << R"({"POSITION":0,"TEXCOORD_0":1,"NORMAL":3},"indices":2,"material":0,"mode":4}]}],)"
             << R"("buffers":[{"uri":"quad.bin","byteLength":140}],)"
             << R"("bufferViews":[{"buffer":0,"byteLength":48},)"
             << R"({"buffer":0,"byteOffset":48,"byteLength":32},)"
             << R"({"buffer":0,"byteOffset":80,"byteLength":12},)"
             << R"({"buffer":0,"byteOffset":92,"byteLength":48}],)"
             << R"("accessors":[{"bufferView":0,"byteOffset":0,"componentType":5126,"count":4,)"
             << R"("type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},)"
             << R"({"bufferView":1,"byteOffset":0,"componentType":5126,"count":4,"type":"VEC2"},)"
             << R"({"bufferView":2,"byteOffset":0,"componentType":5123,"count":6,"type":"SCALAR"},)"
             << R"({"bufferView":3,"byteOffset":0,"componentType":5126,"count":4,"type":"VEC3"}],)"
             << R"("materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1],)"
             << R"("baseColorTexture":{"index":0},"metallicFactor":0.2,"roughnessFactor":0.6}}],)"
             << R"("textures":[{"source":0}],"images":[{"uri":"colors.png"}]})";
        REQUIRE(gltf.good());
    }
    const std::filesystem::path scenePath = runRoot / "scene.json";
    const ar::render::StaticCamera initialCamera{
        {0.F, 0.F, 3.F}, {0.F, 0.F, 0.F, 1.F}, std::numbers::pi_v<float> * 0.5F, 0.1F, 100.F};
    {
        std::ofstream scene(scenePath, std::ios::binary);
        scene << Json{
            {"version", 1},
            {"camera",
             {{"position", {0.0, 0.0, 3.0}},
              {"orientation", {0.0, 0.0, 0.0, 1.0}},
              {"fov_y", std::numbers::pi_v<double> * 0.5},
              {"near", 0.1},
              {"far", 100.0}}},
            {"objects",
             {{{"id", 1},
               {"asset", "models/quad.gltf"},
               {"position", {0.0, 0.0, 0.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {1.0, 1.0, 1.0}}}}}};
        REQUIRE(scene.good());
    }

    runtime::RuntimeConfig config;
    config.mode = runtime::QueryMode::Development;
    config.query.requestedApi = rhi::makeApiVersion(1, 3);
    config.query.createGraphicsDevice = true;
    config.query.enableScreenshot = true;
    config.query.enableSetBinding = true;
    config.query.enableGBuffer = true;
    config.staticAssetRoot = assetRoot;
    config.query.triangleShaders =
        rhi::TriangleShaders{shader("gbuffer.vert.spv"), shader("gbuffer_bindless.frag.spv")};
    config.query.deferredLighting = rhi::DeferredLighting{
        shader("fullscreen.vert.spv"),
        shader("deferred_direct.frag.spv"),
        shader("display_sdr.frag.spv"),
        0.F,
        false};
    if (indirect) {
        config.query.staticIndirect = rhi::StaticIndirectShaders{
            {shader("static_indirect.vert.spv"), shader("static_indirect.frag.spv")},
            shader("static_indirect.comp.spv"),
            UINT32_MAX};
    }
    config.window.emplace();
    config.window->width = 320;
    config.window->height = 180;
    config.window->visible = false;
    config.window->title =
        "AR camera view test " + runSuffix + " " + std::to_string(GetCurrentProcessId());
    config.diagnostics.runId = runId;
    config.diagnostics.outputRoot = runRoot;

    auto session = runtime::RuntimeSession::start(config);
    REQUIRE(session);
    CHECK((*session)->staticSceneUploadCount() == 0);
    REQUIRE((*session)->openStaticScene(scenePath));
    CHECK((*session)->staticSceneUploadCount() == 1);

    const auto presentCapture = [&](rhi::FrameScreenshot& out) {
        auto status = (*session)->presentFrame(&out);
        REQUIRE(status);
        REQUIRE(*status == runtime::FrameStatus::Presented);
        REQUIRE(out.gbuffer.size() == 3);
    };

    // Frame 1: initial camera at (0, 0, 3) looking down -Z.
    rhi::FrameScreenshot frame1;
    presentCapture(frame1);
    const float expectedDepth1 = 0.1F * (100.F - 3.F) / (3.F * (100.F - 0.1F));
    CHECK(frame1.gbuffer[0].depth == Catch::Approx(expectedDepth1).margin(1e-4F));
    CHECK(frame1.gbuffer[0].viewPosition[2] == Catch::Approx(-3.F).margin(0.01F));
    CHECK(frame1.gbuffer[0].normal[0] == Catch::Approx(0.F).margin(0.01F));
    CHECK(frame1.gbuffer[0].normal[1] == Catch::Approx(0.F).margin(0.01F));
    CHECK(frame1.gbuffer[0].normal[2] == Catch::Approx(1.F).margin(0.01F));
    CHECK(frame1.gbuffer[2].depth == 0.F);
    REQUIRE(frame1.gbuffer[0].hdrColor.has_value());
    CHECK((*frame1.gbuffer[0].hdrColor)[0] + (*frame1.gbuffer[0].hdrColor)[2] > 0.F);
    if (indirect) {
        REQUIRE(frame1.indirect.has_value());
        CHECK(frame1.indirect->visible == 1);
        CHECK(frame1.indirect->drawnIds == std::vector<std::uint64_t>{1});
    }

    // Invalid camera is rejected without mutating upload count or active view.
    ar::render::StaticCamera invalidCamera = initialCamera;
    invalidCamera.orientation = {0.F, 0.F, 0.F, 0.F};
    const auto rejectedQuat = (*session)->setCamera(invalidCamera);
    REQUIRE_FALSE(rejectedQuat);
    CHECK(rejectedQuat.error().category == core::ErrorCategory::InvalidArguments);
    invalidCamera = initialCamera;
    invalidCamera.nearPlane = 0.F;
    const auto rejectedNear = (*session)->setCamera(invalidCamera);
    REQUIRE_FALSE(rejectedNear);
    CHECK(rejectedNear.error().category == core::ErrorCategory::InvalidArguments);
    CHECK((*session)->staticSceneUploadCount() == 1);

    // Frame 2: turn 180 deg around +Y (looking away from the quad).
    ar::render::StaticCamera turnedAway = initialCamera;
    turnedAway.orientation = {0.F, 1.F, 0.F, 0.F};
    REQUIRE((*session)->setCamera(turnedAway));
    CHECK((*session)->staticSceneUploadCount() == 1);
    rhi::FrameScreenshot frame2;
    presentCapture(frame2);
    CHECK(frame2.gbuffer[0].depth == 0.F);
    if (indirect) {
        REQUIRE(frame2.indirect.has_value());
        CHECK(frame2.indirect->visible == 0);
        CHECK(frame2.indirect->drawnIds.empty());
    }

    // Frame 3 & 4: orbit +30 deg around +Y at radius 2.0 (exercises both double-buffered slots).
    const float halfAngle = std::numbers::pi_v<float> / 12.F;
    const ar::render::StaticCamera orbitedCloser{
        {1.F, 0.F, std::sqrt(3.F)},
        {0.F, std::sin(halfAngle), 0.F, std::cos(halfAngle)},
        initialCamera.verticalFovRadians,
        initialCamera.nearPlane,
        initialCamera.farPlane};
    REQUIRE((*session)->setCamera(orbitedCloser));
    CHECK((*session)->staticSceneUploadCount() == 1);
    rhi::FrameScreenshot frame3;
    rhi::FrameScreenshot frame4;
    presentCapture(frame3);
    presentCapture(frame4);
    CHECK(frame3.gbuffer[0].depth > frame1.gbuffer[0].depth);
    CHECK(frame3.gbuffer[0].viewPosition[2] > frame1.gbuffer[0].viewPosition[2]);
    CHECK(frame3.gbuffer[0].viewPosition[2] == Catch::Approx(-2.F).margin(0.05F));
    CHECK(frame3.gbuffer[0].normal[0] == Catch::Approx(-0.5F).margin(0.015F));
    CHECK(frame3.gbuffer[0].normal[1] == Catch::Approx(0.F).margin(0.015F));
    CHECK(frame3.gbuffer[0].normal[2] == Catch::Approx(std::sqrt(3.F) * 0.5F).margin(0.015F));
    CHECK(frame4.rgba == frame3.rgba);
    if (indirect) {
        REQUIRE(frame3.indirect.has_value());
        CHECK(frame3.indirect->visible == 1);
        CHECK(frame3.indirect->drawnIds == std::vector<std::uint64_t>{1});
        REQUIRE(frame4.indirect.has_value());
        CHECK(frame4.indirect->visible == 1);
        CHECK(frame4.indirect->drawnIds == std::vector<std::uint64_t>{1});
    }

    // Frame 5: restore initial camera and verify exact pixel & probe parity with Frame 1.
    REQUIRE((*session)->setCamera(initialCamera));
    CHECK((*session)->staticSceneUploadCount() == 1);
    rhi::FrameScreenshot frame5;
    presentCapture(frame5);
    CHECK(frame5.rgba == frame1.rgba);
    CHECK(frame5.gbuffer[0].depth == Catch::Approx(frame1.gbuffer[0].depth).margin(1e-5F));
    CHECK(
        frame5.gbuffer[0].viewPosition[2] ==
        Catch::Approx(frame1.gbuffer[0].viewPosition[2]).margin(1e-3F));
    if (indirect) {
        REQUIRE(frame5.indirect.has_value());
        CHECK(frame5.indirect->visible == 1);
        CHECK(frame5.indirect->drawnIds == std::vector<std::uint64_t>{1});
    }

    const auto closed = (*session)->close();
    REQUIRE(closed.result == core::ResultKind::Success);
    REQUIRE(closed.resourcesReleased);
    CHECK((*session)->staticSceneUploadCount() == 1);

    std::ifstream events(runRoot / runId / "events.jsonl");
    REQUIRE(events);
    bool sawValidationEnabled = false;
    for (std::string line; std::getline(events, line);) {
        const auto id = Json::parse(line).at("event_id").get<std::string>();
        CHECK(id != "rhi.validation.error");
        sawValidationEnabled |= (id == "rhi.validation.enabled");
    }
#ifdef _DEBUG
    CHECK(sawValidationEnabled);
#else
    (void)sawValidationEnabled;
#endif
}

TEST_CASE(
    "Descriptor Heap scene layout and RequireProduct window failure mapping",
    "[rhi-descriptor-heap-scene-layout]") {
    using namespace rhi::detail;

    // Scene Heap slot partitions match design.md: 256 scene + 10 per-frame attachments + 3 IBL.
    STATIC_REQUIRE(kHeapSceneImageLimit == 256u);
    STATIC_REQUIRE(kHeapExternalImageBase == 256u);
    STATIC_REQUIRE(kHeapAttachmentSlotsPerFrame == 5u);
    STATIC_REQUIRE(kHeapFrameSlotCount == 2u);
    STATIC_REQUIRE(kHeapEnvironmentImageBase == 266u);
    STATIC_REQUIRE(kHeapEnvironmentImageCount == 3u);
    STATIC_REQUIRE(kHeapTotalSceneImages == 269u);
    STATIC_REQUIRE(kHeapSceneBufferCount == 8u);
    STATIC_REQUIRE(kHeapSceneSamplerCount == 4u);

    rhi::DescriptorHeapLimits limits{};
    limits.samplerHeapAlignment = 128;
    limits.resourceHeapAlignment = 256;
    limits.maxSamplerHeapSize = 65536;
    limits.maxResourceHeapSize = 1048576;
    limits.minSamplerHeapReservedRange = 256;
    limits.minResourceHeapReservedRange = 512;
    limits.samplerDescriptorSize = 20;
    limits.samplerDescriptorAlignment = 32;
    limits.imageDescriptorSize = 48;
    limits.imageDescriptorAlignment = 32;
    limits.bufferDescriptorSize = 24;
    limits.bufferDescriptorAlignment = 16;
    limits.maxPushDataSize = 128;

    const DescriptorHeapCapacity sceneCapacity{
        kHeapTotalSceneImages, kHeapSceneBufferCount, kHeapSceneSamplerCount};
    const auto plan = planDescriptorHeaps(limits, sceneCapacity);
    REQUIRE(plan.has_value());
    CHECK(plan->imageStride == 64u);
    CHECK(plan->imageRegionSize == 269u * 64u);
    CHECK(plan->bufferStride == 32u);
    CHECK(plan->bufferBaseOffset >= plan->imageRegionSize);
    CHECK((plan->bufferBaseOffset % limits.bufferDescriptorAlignment) == 0u);
    CHECK(plan->bufferRegionSize == 8u * 32u);
    CHECK(
        (kHeapEnvironmentImageBase + kHeapEnvironmentImageCount) * plan->imageStride <=
        plan->bufferBaseOffset);
    CHECK(plan->resourceReservedOffset >= plan->bufferBaseOffset + plan->bufferRegionSize);
    CHECK((plan->resourceReservedOffset % limits.resourceHeapAlignment) == 0u);
    CHECK(
        plan->resourceHeapSize ==
        plan->resourceReservedOffset + limits.minResourceHeapReservedRange);
    CHECK(plan->resourceHeapSize <= limits.maxResourceHeapSize);
    CHECK(plan->samplerStride == 32u);
    CHECK(plan->samplerRegionSize == 4u * 32u);
    CHECK(plan->samplerReservedOffset >= plan->samplerRegionSize);
    CHECK((plan->samplerReservedOffset % limits.samplerHeapAlignment) == 0u);
    CHECK(
        plan->samplerHeapSize == plan->samplerReservedOffset + limits.minSamplerHeapReservedRange);
    CHECK(plan->samplerHeapSize <= limits.maxSamplerHeapSize);

    // Reject tight heap budgets, overflow, and invalid limits/capacities.
    {
        auto tight = limits;
        tight.maxResourceHeapSize = plan->resourceHeapSize - 1u;
        const auto rejected = planDescriptorHeaps(tight, sceneCapacity);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() == DescriptorHeapLayoutError::ResourceHeapExceeded);
    }
    {
        auto tight = limits;
        tight.maxSamplerHeapSize = plan->samplerHeapSize - 1u;
        const auto rejected = planDescriptorHeaps(tight, sceneCapacity);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() == DescriptorHeapLayoutError::SamplerHeapExceeded);
    }
    {
        auto overflow = limits;
        overflow.bufferDescriptorSize = (std::numeric_limits<std::uint64_t>::max)() / 4u;
        overflow.bufferDescriptorAlignment = 1;
        overflow.maxResourceHeapSize = (std::numeric_limits<std::uint64_t>::max)();
        const auto rejected = planDescriptorHeaps(overflow, sceneCapacity);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() == DescriptorHeapLayoutError::Overflow);
    }
    {
        auto badLimits = limits;
        badLimits.maxPushDataSize = 0;
        const auto rejected = planDescriptorHeaps(badLimits, sceneCapacity);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() == DescriptorHeapLayoutError::InvalidLimits);
    }
    {
        const auto rejected = planDescriptorHeaps(limits, {0u, kHeapSceneBufferCount, 4u});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() == DescriptorHeapLayoutError::InvalidCapacity);
    }

    // RHI request validation for Heap scene presentation (T44 scope).
    const std::vector<std::uint32_t> dummySpirv{0x07230203u, 0u, 0u, 0u, 0u};
    {
        const std::string runId = "rhi-descriptor-heap-scene-validation";
        const auto runRoot = freshRunDirectory(runId);
        const core::BuildInfo& build = core::buildInfo();
        core::RunSummaryIdentity identity{
            build.buildId, build.inputHash, build.configuration, build.source};
        auto diagnostics = core::DiagnosticSession::create({runRoot, runId}, identity);
        REQUIRE(diagnostics.has_value());

        const auto makeBase = [&]() {
            rhi::QueryConfig query;
            query.loader = rhi::ExplicitLoaderPath{fixtureModule()};
            query.requestedApi = rhi::kApiVersion1_4;
            query.createGraphicsDevice = true;
            query.enableDescriptorHeap = true;
            query.enableStaticScene = true;
            query.enableGBuffer = true;
            query.presentation = rhi::Win32Presentation{
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(1)),
                {"VK_KHR_surface", "VK_KHR_win32_surface"},
                320u,
                180u};
            query.triangleShaders = rhi::TriangleShaders{dummySpirv, dummySpirv};
            query.planPresentation = ar::render::planPresentationGraph;
            query.presentationSpec = ar::render::buildDeferredPresentationSpec(0.F);
            return query;
        };

        auto bothModes = makeBase();
        bothModes.enableSetBinding = true;
        auto opened = rhi::RhiSession::open(bothModes, **diagnostics);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().error.category == core::ErrorCategory::InvalidArguments);

        auto oldApi = makeBase();
        oldApi.requestedApi = rhi::makeApiVersion(1, 3);
        opened = rhi::RhiSession::open(oldApi, **diagnostics);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().error.category == core::ErrorCategory::InvalidArguments);

        auto noGBuffer = makeBase();
        noGBuffer.enableGBuffer = false;
        noGBuffer.planPresentation = nullptr;
        noGBuffer.presentationSpec.reset();
        opened = rhi::RhiSession::open(noGBuffer, **diagnostics);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().error.category == core::ErrorCategory::InvalidArguments);

        auto noStaticScene = makeBase();
        noStaticScene.enableStaticScene = false;
        noStaticScene.enableGBuffer = false;
        noStaticScene.planPresentation = nullptr;
        noStaticScene.presentationSpec.reset();
        opened = rhi::RhiSession::open(noStaticScene, **diagnostics);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().error.category == core::ErrorCategory::InvalidArguments);

        // T45: Heap accepts deferredLighting + staticIndirect when GBuffer and shaders are valid;
        // invalid shader / oversized display push / missing GBuffer are still rejected.
        auto badIndirectShader = makeBase();
        badIndirectShader.staticIndirect =
            rhi::StaticIndirectShaders{{dummySpirv, {0x12345678u}}, dummySpirv, UINT32_MAX};
        opened = rhi::RhiSession::open(badIndirectShader, **diagnostics);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().error.category == core::ErrorCategory::InvalidArguments);

        auto badDeferredNoGBuffer = makeBase();
        badDeferredNoGBuffer.enableGBuffer = false;
        badDeferredNoGBuffer.deferredLighting =
            rhi::DeferredLighting{dummySpirv, dummySpirv, dummySpirv, 0.F, false};
        opened = rhi::RhiSession::open(badDeferredNoGBuffer, **diagnostics);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().error.category == core::ErrorCategory::InvalidArguments);

        auto oversizedHeapDisplayPush = makeBase();
        oversizedHeapDisplayPush.deferredLighting =
            rhi::DeferredLighting{dummySpirv, dummySpirv, dummySpirv, 0.F, false};
        oversizedHeapDisplayPush.presentationSpec->buildDisplayPush = [](float, bool) noexcept {
            rhi::DisplayPushData data{};
            data.size = 12u;
            return data;
        };
        opened = rhi::RhiSession::open(oversizedHeapDisplayPush, **diagnostics);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().error.category == core::ErrorCategory::InvalidArguments);

        core::RunSummaryInput summary;
        summary.runId = runId;
        summary.identity = identity;
        summary.primaryResult = core::ResultKind::Success;
        summary.resourcesReleased = true;
        CHECK((*diagnostics)->finish(summary).diagnosticsComplete);
    }

    // Runtime mode validation: Inspect + window and RequireProduct + window without device fail
    // before touching the Vulkan loader.
    {
        runtime::RuntimeConfig noDevice =
            fixtureConfig("rhi-heap-window-no-device", "devices=1;device=1.4");
        noDevice.mode = runtime::QueryMode::RequireProduct;
        noDevice.query.requestedApi = rhi::kApiVersion1_4;
        noDevice.query.createGraphicsDevice = false;
        noDevice.window.emplace();
        noDevice.window->visible = false;
        rhi::detail::resetProcessCounters();
        auto started = runtime::RuntimeSession::start(noDevice);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().endResult.result == core::ResultKind::InvalidArguments);
        CHECK(rhi::detail::processCounters().loadCalls == 0);

        runtime::RuntimeConfig inspectWindow =
            fixtureConfig("rhi-heap-window-inspect", "devices=1;device=1.4");
        inspectWindow.mode = runtime::QueryMode::Inspect;
        inspectWindow.query.requestedApi = rhi::kApiVersion1_4;
        inspectWindow.query.createGraphicsDevice = true;
        inspectWindow.window.emplace();
        inspectWindow.window->visible = false;
        rhi::detail::resetProcessCounters();
        started = runtime::RuntimeSession::start(inspectWindow);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().endResult.result == core::ResultKind::InvalidArguments);
        CHECK(rhi::detail::processCounters().loadCalls == 0);
    }

    const auto makeHeapWindowConfig = [&](const std::string& runId, const char* fixtureSpec) {
        runtime::RuntimeConfig config = fixtureConfig(runId, fixtureSpec);
        config.mode = runtime::QueryMode::RequireProduct;
        config.query.requestedApi = rhi::kApiVersion1_4;
        config.query.createGraphicsDevice = true;
        config.query.enableDescriptorHeap = true;
        config.query.enableStaticScene = true;
        config.query.enableGBuffer = true;
        config.query.triangleShaders = rhi::TriangleShaders{dummySpirv, dummySpirv};
        config.window.emplace();
        config.window->width = 320;
        config.window->height = 180;
        config.window->visible = false;
        config.window->title = "AR heap failure " + runId;
        return config;
    };

    const auto verifyWindowCanReopen = [](const char* title) {
        ar::platform::WindowConfig checkWindow;
        checkWindow.width = 160;
        checkWindow.height = 90;
        checkWindow.visible = false;
        checkWindow.title = title;
        auto reopened = ar::platform::Window::open(checkWindow);
        INFO((reopened ? "" : reopened.error().message));
        REQUIRE(reopened.has_value());
        reopened->reset();
    };

    // Failure mapping 1: RequireProduct window on Vulkan 1.3 device -> ApiVersionUnsupported
    // (also confirms deferredLighting + staticIndirect passes T45 Heap request validation).
    {
        auto config = makeHeapWindowConfig(
            "rhi-heap-window-api13", "loader=1.4;device=1.3;devices=1;surface=ok");
        config.query.deferredLighting =
            rhi::DeferredLighting{dummySpirv, dummySpirv, dummySpirv, 0.F, true};
        config.query.staticIndirect =
            rhi::StaticIndirectShaders{{dummySpirv, dummySpirv}, dummySpirv, UINT32_MAX};
        auto started = runtime::RuntimeSession::start(config);
        REQUIRE_FALSE(started.has_value());
        const auto& failure = started.error();
        CHECK(failure.endResult.result == core::ResultKind::Unsupported);
        REQUIRE(failure.endResult.primaryError.has_value());
        CHECK(
            failure.endResult.primaryError->category == core::ErrorCategory::ApiVersionUnsupported);
        CHECK(failure.endResult.resourcesReleased);
        CHECK(failure.endResult.residualResources.empty());
        CHECK(failure.endResult.cleanupErrors.empty());
        verifyWindowCanReopen("AR window release after api13");
    }

    // Failure mapping 2: Vulkan 1.4 device missing VK_EXT_descriptor_heap -> preserve QueryFailed.
    {
        const auto config = makeHeapWindowConfig(
            "rhi-heap-window-no-heap", "loader=1.4;device=1.4;devices=1;surface=ok;heap=absent");
        auto started = runtime::RuntimeSession::start(config);
        REQUIRE_FALSE(started.has_value());
        const auto& failure = started.error();
        CHECK(failure.endResult.result == core::ResultKind::InitializationFailed);
        REQUIRE(failure.endResult.primaryError.has_value());
        CHECK(failure.endResult.primaryError->category == core::ErrorCategory::QueryFailed);
        CHECK(
            failure.endResult.primaryError->message.find("VK_EXT_descriptor_heap") !=
            std::string::npos);
        CHECK(failure.endResult.resourcesReleased);
        CHECK(failure.endResult.residualResources.empty());
        CHECK(failure.endResult.cleanupErrors.empty());
        verifyWindowCanReopen("AR window release after missing heap");
    }

    // Failure mapping 3: Vulkan 1.4 device with invalid Heap limits or missing entry point.
    {
        const auto config = makeHeapWindowConfig(
            "rhi-heap-window-zero-limits",
            "loader=1.4;device=1.4;devices=1;surface=ok;heap=present;heap-limits=zero");
        auto started = runtime::RuntimeSession::start(config);
        REQUIRE_FALSE(started.has_value());
        const auto& failure = started.error();
        CHECK(failure.endResult.result == core::ResultKind::InitializationFailed);
        REQUIRE(failure.endResult.primaryError.has_value());
        CHECK(failure.endResult.primaryError->category == core::ErrorCategory::QueryFailed);
        CHECK(failure.endResult.resourcesReleased);
        CHECK(failure.endResult.residualResources.empty());
        verifyWindowCanReopen("AR window release after zero limits");
    }
    {
        const auto config = makeHeapWindowConfig(
            "rhi-heap-window-missing-entry",
            "loader=1.4;device=1.4;devices=1;surface=ok;heap=present;heap-entry=missing-push-data");
        auto started = runtime::RuntimeSession::start(config);
        REQUIRE_FALSE(started.has_value());
        const auto& failure = started.error();
        CHECK(failure.endResult.result == core::ResultKind::InitializationFailed);
        REQUIRE(failure.endResult.primaryError.has_value());
        CHECK(failure.endResult.primaryError->category == core::ErrorCategory::LoaderSymbolMissing);
        CHECK(failure.endResult.resourcesReleased);
        CHECK(failure.endResult.residualResources.empty());
        verifyWindowCanReopen("AR window release after missing entry");
    }

    // Failure mapping 4: Post-open RequireProduct rejection when API is 1.4 but required
    // capability extension is absent -> QueryFailed (not ApiVersionUnsupported).
    {
        runtime::RuntimeConfig config = fixtureConfig(
            "rhi-heap-require-product-capability", "loader=1.4;device=1.4;devices=1;heap=absent");
        config.mode = runtime::QueryMode::RequireProduct;
        config.query.requestedApi = rhi::kApiVersion1_4;
        config.query.requiredDeviceExtensions = {"VK_EXT_descriptor_heap"};
        auto started = runtime::RuntimeSession::start(config);
        REQUIRE_FALSE(started.has_value());
        const auto& failure = started.error();
        CHECK(failure.endResult.result == core::ResultKind::InitializationFailed);
        REQUIRE(failure.endResult.primaryError.has_value());
        CHECK(failure.endResult.primaryError->category == core::ErrorCategory::QueryFailed);
        CHECK(failure.report.judgments.apiVersionSupported);
        CHECK_FALSE(failure.report.judgments.requiredCapabilitiesSupported);
        CHECK(failure.endResult.resourcesReleased);
        CHECK(failure.endResult.residualResources.empty());
    }
}

TEST_CASE(
    "Direct bindless GBuffer Descriptor Heap path matches Descriptor Set baseline",
    "[sample-static-scene-heap-gbuffer]") {
    const auto shader = [](const char* name) {
        std::ifstream input(
            std::filesystem::path(AR_TEST_SHADER_DIR) / name, std::ios::binary | std::ios::ate);
        const auto size = static_cast<std::streamoff>(input.tellg());
        REQUIRE(size > 0);
        REQUIRE(size % 4 == 0);
        std::vector<std::uint32_t> words(static_cast<std::size_t>(size / 4));
        REQUIRE(input.seekg(0).read(reinterpret_cast<char*>(words.data()), size));
        return words;
    };

    const std::string baseRunId = "sample-static-scene-heap-gbuffer";
    const std::filesystem::path runRoot = freshRunDirectory(baseRunId);
    const std::filesystem::path assetRoot = runRoot / "assets";
    const std::filesystem::path modelsDir = assetRoot / "models";
    std::filesystem::create_directories(modelsDir);

    // Probe real GPU Vulkan 1.4 + VK_EXT_descriptor_heap capability before running the comparison.
    bool supportsHeap = false;
    {
        runtime::RuntimeConfig probe;
        probe.mode = runtime::QueryMode::Inspect;
        probe.query.loader = rhi::SystemLoader{};
        probe.query.requestedApi = rhi::makeApiVersion(1, 3);
        probe.query.device = rhi::ExplicitDeviceIndex{0};
        probe.diagnostics.outputRoot = runRoot;
        probe.diagnostics.runId = "probe";
        auto probed = runtime::RuntimeSession::start(probe);
        INFO(describeStart(probed));
        REQUIRE(probed.has_value());
        const auto& report = (*probed)->report();
        supportsHeap = report.loaderApi >= rhi::kApiVersion1_4 &&
                       report.deviceApi >= rhi::kApiVersion1_4 &&
                       report.descriptorHeap == rhi::DescriptorHeapStatus::Supported;
        REQUIRE((*probed)->close().resourcesReleased);
    }

    // Two distinct textured meshes placed over Probe 0 (0.5, 0.38) and Probe 1 (0.5, 0.76) while
    // leaving Probe 2 (0.05, 0.05) on clear background, exercising multi-slot bindless indexing.
    const std::array<float, 12> positions{
        -1.F, -1.F, 0.F, 1.F, -1.F, 0.F, 1.F, 1.F, 0.F, -1.F, 1.F, 0.F};
    const std::array<float, 8> uvs{0.F, 0.F, 1.F, 0.F, 1.F, 1.F, 0.F, 1.F};
    const std::array<std::uint16_t, 6> indices{0, 1, 2, 0, 2, 3};
    const std::array<float, 12> normals{0.F, 0.F, 1.F, 0.F, 0.F, 1.F, 0.F, 0.F, 1.F, 0.F, 0.F, 1.F};
    constexpr std::array<unsigned char, 71> redBluePng{
        137, 80,  78, 71,  13,  10,  26,  10, 0,   0,   0,   13,  73, 72,  68,  82, 0,   0,
        0,   2,   0,  0,   0,   1,   8,   6,  0,   0,   0,   244, 34, 127, 138, 0,  0,   0,
        14,  73,  68, 65,  84,  120, 156, 99, 248, 207, 192, 0,   66, 255, 1,   15, 249, 3,
        253, 133, 17, 153, 118, 0,   0,   0,  0,   73,  69,  78,  68, 174, 66,  96, 130};
    constexpr std::array<unsigned char, 70> greenPng{
        137, 80,  78, 71,  13, 10,  26,  10, 0,  0,   0,   13,  73,  72,  68,  82, 0, 0,
        0,   1,   0,  0,   0,  1,   8,   6,  0,  0,   0,   31,  21,  196, 137, 0,  0, 0,
        13,  73,  68, 65,  84, 120, 156, 99, 96, 248, 207, 240, 31,  0,   4,   1,  1, 255,
        113, 235, 71, 229, 0,  0,   0,   0,  73, 69,  78,  68,  174, 66,  96,  130};

    {
        std::ofstream bin(modelsDir / "quad.bin", std::ios::binary);
        bin.write(
            reinterpret_cast<const char*>(positions.data()),
            static_cast<std::streamsize>(sizeof(positions)));
        bin.write(
            reinterpret_cast<const char*>(uvs.data()), static_cast<std::streamsize>(sizeof(uvs)));
        bin.write(
            reinterpret_cast<const char*>(indices.data()),
            static_cast<std::streamsize>(sizeof(indices)));
        bin.write(
            reinterpret_cast<const char*>(normals.data()),
            static_cast<std::streamsize>(sizeof(normals)));
        REQUIRE(bin.good());
    }
    {
        std::ofstream img(modelsDir / "red_blue.png", std::ios::binary);
        img.write(
            reinterpret_cast<const char*>(redBluePng.data()),
            static_cast<std::streamsize>(sizeof(redBluePng)));
        REQUIRE(img.good());
    }
    {
        std::ofstream img(modelsDir / "green.png", std::ios::binary);
        img.write(
            reinterpret_cast<const char*>(greenPng.data()),
            static_cast<std::streamsize>(sizeof(greenPng)));
        REQUIRE(img.good());
    }
    const auto writeGltf = [&](const char* fileName,
                               const char* imageName,
                               const char* metallic,
                               const char* roughness) {
        std::ofstream gltf(modelsDir / fileName, std::ios::binary);
        gltf << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
             << R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":)"
             << R"({"POSITION":0,"TEXCOORD_0":1,"NORMAL":3},"indices":2,"material":0,"mode":4}]}],)"
             << R"("buffers":[{"uri":"quad.bin","byteLength":140}],)"
             << R"("bufferViews":[{"buffer":0,"byteLength":48},)"
             << R"({"buffer":0,"byteOffset":48,"byteLength":32},)"
             << R"({"buffer":0,"byteOffset":80,"byteLength":12},)"
             << R"({"buffer":0,"byteOffset":92,"byteLength":48}],)"
             << R"("accessors":[{"bufferView":0,"byteOffset":0,"componentType":5126,"count":4,)"
             << R"("type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},)"
             << R"({"bufferView":1,"byteOffset":0,"componentType":5126,"count":4,"type":"VEC2"},)"
             << R"({"bufferView":2,"byteOffset":0,"componentType":5123,"count":6,"type":"SCALAR"},)"
             << R"({"bufferView":3,"byteOffset":0,"componentType":5126,"count":4,"type":"VEC3"}],)"
             << R"("materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1],)"
             << R"("baseColorTexture":{"index":0},"metallicFactor":)" << metallic
             << R"(,"roughnessFactor":)" << roughness << R"(}}],)"
             << R"("textures":[{"source":0}],"images":[{"uri":")" << imageName << R"("}]})";
        REQUIRE(gltf.good());
    };
    writeGltf("upper.gltf", "red_blue.png", "0.25", "0.75");
    writeGltf("lower.gltf", "green.png", "0.85", "0.35");

    const std::filesystem::path scenePath = runRoot / "scene.json";
    const ar::render::StaticCamera initialCamera{
        {0.F, 0.F, 3.F}, {0.F, 0.F, 0.F, 1.F}, std::numbers::pi_v<float> * 0.5F, 0.1F, 100.F};
    const float halfAngle = std::numbers::pi_v<float> / 18.F;
    const ar::render::StaticCamera orbitedCamera{
        {0.6F, 0.F, 2.6F},
        {0.F, std::sin(halfAngle), 0.F, std::cos(halfAngle)},
        initialCamera.verticalFovRadians,
        initialCamera.nearPlane,
        initialCamera.farPlane};
    {
        std::ofstream scene(scenePath, std::ios::binary);
        scene << Json{
            {"version", 1},
            {"camera",
             {{"position", {0.0, 0.0, 3.0}},
              {"orientation", {0.0, 0.0, 0.0, 1.0}},
              {"fov_y", std::numbers::pi_v<double> * 0.5},
              {"near", 0.1},
              {"far", 100.0}}},
            {"objects",
             {{{"id", 1},
               {"asset", "models/upper.gltf"},
               {"position", {0.0, 0.72, 0.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {1.0, 0.5, 1.0}}},
              {{"id", 2},
               {"asset", "models/lower.gltf"},
               {"position", {0.0, -2.08, -1.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {1.2, 0.7, 1.0}}}}}};
        REQUIRE(scene.good());
    }

    const rhi::TriangleShaders directShaders{
        shader("gbuffer.vert.spv"), shader("gbuffer_bindless.frag.spv")};

    struct RunResult {
        rhi::FrameScreenshot initial;
        rhi::FrameScreenshot orbited1;
        rhi::FrameScreenshot orbited2;
        rhi::FrameScreenshot restored;
    };

    const auto makeConfig = [&](bool heapMode, const std::string& runId) {
        runtime::RuntimeConfig config;
        config.mode =
            heapMode ? runtime::QueryMode::RequireProduct : runtime::QueryMode::Development;
        config.query.loader = rhi::SystemLoader{};
        config.query.device = rhi::ExplicitDeviceIndex{0};
        config.query.requestedApi = heapMode ? rhi::kApiVersion1_4 : rhi::makeApiVersion(1, 3);
        config.query.createGraphicsDevice = true;
        config.query.enableScreenshot = true;
        config.query.enableSetBinding = !heapMode;
        config.query.enableDescriptorHeap = heapMode;
        config.query.enableGBuffer = true;
        config.staticAssetRoot = assetRoot;
        config.query.triangleShaders = directShaders;
        config.window.emplace();
        config.window->width = 320;
        config.window->height = 180;
        config.window->visible = false;
        config.window->title =
            "AR heap gbuffer " + runId + " " + std::to_string(GetCurrentProcessId());
        config.diagnostics.runId = runId;
        config.diagnostics.outputRoot = runRoot;
        return config;
    };

    const auto runPath = [&](bool heapMode, const std::string& runId) -> RunResult {
        const auto config = makeConfig(heapMode, runId);
        auto session = runtime::RuntimeSession::start(config);
        INFO(describeStart(session));
        REQUIRE(session.has_value());
        CHECK((*session)->staticSceneUploadCount() == 0);
        REQUIRE((*session)->openStaticScene(scenePath));
        CHECK((*session)->staticSceneUploadCount() == 1);

        const auto capture = [&](rhi::FrameScreenshot& out) {
            auto status = (*session)->presentFrame(&out);
            INFO((status ? "" : status.error().message));
            REQUIRE(status.has_value());
            REQUIRE(*status == runtime::FrameStatus::Presented);
            REQUIRE(out.gbuffer.size() == 3);
        };

        RunResult result;
        capture(result.initial);
        REQUIRE((*session)->setCamera(orbitedCamera));
        CHECK((*session)->staticSceneUploadCount() == 1);
        capture(result.orbited1);
        capture(result.orbited2);
        REQUIRE((*session)->setCamera(initialCamera));
        CHECK((*session)->staticSceneUploadCount() == 1);
        capture(result.restored);

        const auto closed = (*session)->close();
        REQUIRE(closed.result == core::ResultKind::Success);
        REQUIRE(closed.resourcesReleased);
        CHECK((*session)->staticSceneUploadCount() == 1);

        std::ifstream events(runRoot / runId / "events.jsonl");
        REQUIRE(events);
        bool sawValidationEnabled = false;
        bool sawHeapReady = false;
        bool sawSetReady = false;
        for (std::string line; std::getline(events, line);) {
            const auto id = Json::parse(line).at("event_id").get<std::string>();
            CHECK(id != "rhi.validation.error");
            sawValidationEnabled |= (id == "rhi.validation.enabled");
            sawHeapReady |= (id == "rhi.binding.heap.ready");
            sawSetReady |= (id == "rhi.binding.set.ready");
        }
#ifdef _DEBUG
        CHECK(sawValidationEnabled);
#else
        (void)sawValidationEnabled;
#endif
        CHECK(sawHeapReady == heapMode);
        CHECK(sawSetReady == !heapMode);
        return result;
    };

    if (!supportsHeap) {
        auto rejected = runtime::RuntimeSession::start(makeConfig(true, "heap-unsupported"));
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().endResult.resourcesReleased);
        CHECK(rejected.error().endResult.residualResources.empty());
        return;
    }

    const RunResult setResult = runPath(false, "set-gbuffer");
    const RunResult heapResult = runPath(true, "heap-gbuffer");

    // Sanity-check concrete physical GBuffer probe values on both materials and background.
    const float expectedDepthUpper = 0.1F * (100.F - 3.F) / (3.F * (100.F - 0.1F));
    const float expectedDepthLower = 0.1F * (100.F - 4.F) / (4.F * (100.F - 0.1F));
    CHECK(setResult.initial.gbuffer[0].depth == Catch::Approx(expectedDepthUpper).margin(1e-4F));
    CHECK(setResult.initial.gbuffer[1].depth == Catch::Approx(expectedDepthLower).margin(1e-4F));
    CHECK(setResult.initial.gbuffer[0].depth > setResult.initial.gbuffer[1].depth);
    CHECK(setResult.initial.gbuffer[2].depth == 0.F);
    CHECK(setResult.initial.gbuffer[0].material[0] == Catch::Approx(0.25F).margin(0.02F));
    CHECK(setResult.initial.gbuffer[0].material[1] == Catch::Approx(0.75F).margin(0.02F));
    CHECK(setResult.initial.gbuffer[1].material[0] == Catch::Approx(0.85F).margin(0.02F));
    CHECK(setResult.initial.gbuffer[1].material[1] == Catch::Approx(0.35F).margin(0.02F));
    CHECK(setResult.initial.gbuffer[1].baseColor[1] > 0.9F);
    CHECK(setResult.initial.gbuffer[1].baseColor[0] < 0.05F);
    CHECK(setResult.initial.gbuffer[1].baseColor[2] < 0.05F);
    CHECK(setResult.orbited1.rgba != setResult.initial.rgba);
    CHECK(setResult.orbited2.rgba == setResult.orbited1.rgba);
    CHECK(setResult.restored.rgba == setResult.initial.rgba);

    const auto checkParity = [](const rhi::FrameScreenshot& expected,
                                const rhi::FrameScreenshot& actual) {
        CHECK(actual.width == expected.width);
        CHECK(actual.height == expected.height);
        CHECK(actual.rgba == expected.rgba);
        REQUIRE(actual.gbuffer.size() == expected.gbuffer.size());
        for (std::size_t i = 0; i < expected.gbuffer.size(); ++i) {
            const auto& lhs = expected.gbuffer[i];
            const auto& rhs = actual.gbuffer[i];
            CHECK(rhs.pixel == lhs.pixel);
            CHECK(rhs.depth == Catch::Approx(lhs.depth).margin(1e-5F));
            for (std::size_t c = 0; c < 4; ++c) {
                CHECK(rhs.baseColor[c] == Catch::Approx(lhs.baseColor[c]).margin(2.F / 255.F));
            }
            for (std::size_t c = 0; c < 3; ++c) {
                CHECK(rhs.normal[c] == Catch::Approx(lhs.normal[c]).margin(1e-4F));
                CHECK(rhs.viewPosition[c] == Catch::Approx(lhs.viewPosition[c]).margin(1e-3F));
            }
            for (std::size_t c = 0; c < 2; ++c) {
                CHECK(rhs.material[c] == Catch::Approx(lhs.material[c]).margin(2.F / 255.F));
            }
        }
    };

    checkParity(setResult.initial, heapResult.initial);
    checkParity(setResult.orbited1, heapResult.orbited1);
    checkParity(setResult.orbited2, heapResult.orbited2);
    checkParity(setResult.restored, heapResult.restored);
}

TEST_CASE(
    "GPU-driven indirect draw and deferred IBL/Display Descriptor Heap path matches Set baseline",
    "[sample-static-scene-heap-gpu-driven]") {
    const auto shader = [](const char* name) {
        std::ifstream input(
            std::filesystem::path(AR_TEST_SHADER_DIR) / name, std::ios::binary | std::ios::ate);
        const auto size = static_cast<std::streamoff>(input.tellg());
        REQUIRE(size > 0);
        REQUIRE(size % 4 == 0);
        std::vector<std::uint32_t> words(static_cast<std::size_t>(size / 4));
        REQUIRE(input.seekg(0).read(reinterpret_cast<char*>(words.data()), size));
        return words;
    };

    const std::string baseRunId = "sample-static-scene-heap-gpu-driven";
    const std::filesystem::path runRoot = freshRunDirectory(baseRunId);
    const std::filesystem::path assetRoot = runRoot / "assets";
    const std::filesystem::path modelsDir = assetRoot / "models";
    std::filesystem::create_directories(modelsDir);

    bool supportsHeap = false;
    {
        runtime::RuntimeConfig probe;
        probe.mode = runtime::QueryMode::Inspect;
        probe.query.loader = rhi::SystemLoader{};
        probe.query.requestedApi = rhi::makeApiVersion(1, 3);
        probe.query.device = rhi::ExplicitDeviceIndex{0};
        probe.diagnostics.outputRoot = runRoot;
        probe.diagnostics.runId = "probe";
        auto probed = runtime::RuntimeSession::start(probe);
        INFO(describeStart(probed));
        REQUIRE(probed.has_value());
        const auto& report = (*probed)->report();
        supportsHeap = report.loaderApi >= rhi::kApiVersion1_4 &&
                       report.deviceApi >= rhi::kApiVersion1_4 &&
                       report.descriptorHeap == rhi::DescriptorHeapStatus::Supported;
        REQUIRE((*probed)->close().resourcesReleased);
    }

    const std::array<float, 12> positions{
        -1.F, -1.F, 0.F, 1.F, -1.F, 0.F, 1.F, 1.F, 0.F, -1.F, 1.F, 0.F};
    const std::array<float, 8> uvs{0.F, 0.F, 1.F, 0.F, 1.F, 1.F, 0.F, 1.F};
    const std::array<std::uint16_t, 6> indices{0, 1, 2, 0, 2, 3};
    const std::array<float, 12> normals{0.F, 0.F, 1.F, 0.F, 0.F, 1.F, 0.F, 0.F, 1.F, 0.F, 0.F, 1.F};
    constexpr std::array<unsigned char, 71> redBluePng{
        137, 80,  78, 71,  13,  10,  26,  10, 0,   0,   0,   13,  73, 72,  68,  82, 0,   0,
        0,   2,   0,  0,   0,   1,   8,   6,  0,   0,   0,   244, 34, 127, 138, 0,  0,   0,
        14,  73,  68, 65,  84,  120, 156, 99, 248, 207, 192, 0,   66, 255, 1,   15, 249, 3,
        253, 133, 17, 153, 118, 0,   0,   0,  0,   73,  69,  78,  68, 174, 66,  96, 130};
    constexpr std::array<unsigned char, 70> greenPng{
        137, 80,  78, 71,  13, 10,  26,  10, 0,  0,   0,   13,  73,  72,  68,  82, 0, 0,
        0,   1,   0,  0,   0,  1,   8,   6,  0,  0,   0,   31,  21,  196, 137, 0,  0, 0,
        13,  73,  68, 65,  84, 120, 156, 99, 96, 248, 207, 240, 31,  0,   4,   1,  1, 255,
        113, 235, 71, 229, 0,  0,   0,   0,  73, 69,  78,  68,  174, 66,  96,  130};

    {
        std::ofstream bin(modelsDir / "quad.bin", std::ios::binary);
        bin.write(
            reinterpret_cast<const char*>(positions.data()),
            static_cast<std::streamsize>(sizeof(positions)));
        bin.write(
            reinterpret_cast<const char*>(uvs.data()), static_cast<std::streamsize>(sizeof(uvs)));
        bin.write(
            reinterpret_cast<const char*>(indices.data()),
            static_cast<std::streamsize>(sizeof(indices)));
        bin.write(
            reinterpret_cast<const char*>(normals.data()),
            static_cast<std::streamsize>(sizeof(normals)));
        REQUIRE(bin.good());
    }
    {
        std::ofstream img(modelsDir / "red_blue.png", std::ios::binary);
        img.write(
            reinterpret_cast<const char*>(redBluePng.data()),
            static_cast<std::streamsize>(sizeof(redBluePng)));
        REQUIRE(img.good());
    }
    {
        std::ofstream img(modelsDir / "green.png", std::ios::binary);
        img.write(
            reinterpret_cast<const char*>(greenPng.data()),
            static_cast<std::streamsize>(sizeof(greenPng)));
        REQUIRE(img.good());
    }
    const auto writeGltf = [&](const char* fileName,
                               const char* imageName,
                               const char* metallic,
                               const char* roughness) {
        std::ofstream gltf(modelsDir / fileName, std::ios::binary);
        gltf << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
             << R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":)"
             << R"({"POSITION":0,"TEXCOORD_0":1,"NORMAL":3},"indices":2,"material":0,"mode":4}]}],)"
             << R"("buffers":[{"uri":"quad.bin","byteLength":140}],)"
             << R"("bufferViews":[{"buffer":0,"byteLength":48},)"
             << R"({"buffer":0,"byteOffset":48,"byteLength":32},)"
             << R"({"buffer":0,"byteOffset":80,"byteLength":12},)"
             << R"({"buffer":0,"byteOffset":92,"byteLength":48}],)"
             << R"("accessors":[{"bufferView":0,"byteOffset":0,"componentType":5126,"count":4,)"
             << R"("type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},)"
             << R"({"bufferView":1,"byteOffset":0,"componentType":5126,"count":4,"type":"VEC2"},)"
             << R"({"bufferView":2,"byteOffset":0,"componentType":5123,"count":6,"type":"SCALAR"},)"
             << R"({"bufferView":3,"byteOffset":0,"componentType":5126,"count":4,"type":"VEC3"}],)"
             << R"("materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1],)"
             << R"("baseColorTexture":{"index":0},"metallicFactor":)" << metallic
             << R"(,"roughnessFactor":)" << roughness << R"(}}],)"
             << R"("textures":[{"source":0}],"images":[{"uri":")" << imageName << R"("}]})";
        REQUIRE(gltf.good());
    };
    // Dielectric upper quad + smooth metallic lower quad make Constant vs. Directional IBL
    // distinct.
    writeGltf("upper.gltf", "red_blue.png", "0.15", "0.65");
    writeGltf("lower.gltf", "green.png", "0.95", "0.15");

    const ar::render::StaticCamera initialCamera{
        {0.F, 0.F, 3.F}, {0.F, 0.F, 0.F, 1.F}, std::numbers::pi_v<float> * 0.5F, 0.1F, 100.F};
    ar::render::StaticCamera turnedAway = initialCamera;
    turnedAway.orientation = {0.F, 1.F, 0.F, 0.F};

    // Scene A (non-overflow): 3 visible objects (IDs 1, 2, 3) + 1 off-frustum culled object (ID 4).
    const std::filesystem::path mainScenePath = runRoot / "main_scene.json";
    {
        std::ofstream scene(mainScenePath, std::ios::binary);
        scene << Json{
            {"version", 1},
            {"camera",
             {{"position", {0.0, 0.0, 3.0}},
              {"orientation", {0.0, 0.0, 0.0, 1.0}},
              {"fov_y", std::numbers::pi_v<double> * 0.5},
              {"near", 0.1},
              {"far", 100.0}}},
            {"objects",
             {{{"id", 1},
               {"asset", "models/upper.gltf"},
               {"position", {0.0, 0.72, 0.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {1.0, 0.5, 1.0}}},
              {{"id", 2},
               {"asset", "models/lower.gltf"},
               {"position", {0.0, -2.08, -1.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {1.2, 0.7, 1.0}}},
              {{"id", 3},
               {"asset", "models/upper.gltf"},
               {"position", {1.8, 0.0, -0.5}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {0.4, 0.4, 1.0}}},
              {{"id", 4},
               {"asset", "models/lower.gltf"},
               {"position", {0.0, 200.0, 0.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {1.0, 1.0, 1.0}}}}}};
        REQUIRE(scene.good());
    }

    // Scene B (capacity overflow): 4 visible objects (IDs 1..4) + 1 culled object (ID 5).
    const std::filesystem::path overflowScenePath = runRoot / "overflow_scene.json";
    {
        std::ofstream scene(overflowScenePath, std::ios::binary);
        scene << Json{
            {"version", 1},
            {"camera",
             {{"position", {0.0, 0.0, 3.0}},
              {"orientation", {0.0, 0.0, 0.0, 1.0}},
              {"fov_y", std::numbers::pi_v<double> * 0.5},
              {"near", 0.1},
              {"far", 100.0}}},
            {"objects",
             {{{"id", 1},
               {"asset", "models/upper.gltf"},
               {"position", {-1.2, 0.6, 0.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {0.4, 0.4, 1.0}}},
              {{"id", 2},
               {"asset", "models/lower.gltf"},
               {"position", {1.2, 0.6, 0.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {0.4, 0.4, 1.0}}},
              {{"id", 3},
               {"asset", "models/upper.gltf"},
               {"position", {-1.2, -0.6, 0.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {0.4, 0.4, 1.0}}},
              {{"id", 4},
               {"asset", "models/lower.gltf"},
               {"position", {1.2, -0.6, 0.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {0.4, 0.4, 1.0}}},
              {{"id", 5},
               {"asset", "models/upper.gltf"},
               {"position", {0.0, 0.0, 15.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {1.0, 1.0, 1.0}}}}}};
        REQUIRE(scene.good());
    }

    const auto constantPacked = ar::render::packEnvironmentLighting(
        ar::render::bakeEnvironmentLighting(ar::render::EnvironmentPreset::Constant));
    const auto directionalPacked = ar::render::packEnvironmentLighting(
        ar::render::bakeEnvironmentLighting(ar::render::EnvironmentPreset::Directional));
    const rhi::EnvironmentUpload constantEnv{
        constantPacked.diffuse, constantPacked.specular, constantPacked.brdf};
    const rhi::EnvironmentUpload directionalEnv{
        directionalPacked.diffuse, directionalPacked.specular, directionalPacked.brdf};

    const rhi::TriangleShaders directShaders{
        shader("gbuffer.vert.spv"), shader("gbuffer_bindless.frag.spv")};
    const rhi::StaticIndirectShaders indirectShadersBase{
        {shader("static_indirect.vert.spv"), shader("static_indirect.frag.spv")},
        shader("static_indirect.comp.spv"),
        UINT32_MAX};
    const rhi::DeferredLighting iblLighting{
        shader("fullscreen.vert.spv"),
        shader("deferred_ibl.frag.spv"),
        shader("display_sdr.frag.spv"),
        0.F,
        true};
    const rhi::DeferredLighting directLighting{
        shader("fullscreen.vert.spv"),
        shader("deferred_direct.frag.spv"),
        shader("display_sdr.frag.spv"),
        0.F,
        false};

    const auto makeConfig =
        [&](bool heapMode, const std::string& runId, bool useIbl, std::uint32_t capacity) {
            runtime::RuntimeConfig config;
            config.mode =
                heapMode ? runtime::QueryMode::RequireProduct : runtime::QueryMode::Development;
            config.query.loader = rhi::SystemLoader{};
            config.query.device = rhi::ExplicitDeviceIndex{0};
            config.query.requestedApi = heapMode ? rhi::kApiVersion1_4 : rhi::makeApiVersion(1, 3);
            config.query.createGraphicsDevice = true;
            config.query.enableScreenshot = true;
            config.query.enableSetBinding = !heapMode;
            config.query.enableDescriptorHeap = heapMode;
            config.query.enableGBuffer = true;
            config.staticAssetRoot = assetRoot;
            config.query.triangleShaders = directShaders;
            config.query.deferredLighting = useIbl ? iblLighting : directLighting;
            config.query.staticIndirect = indirectShadersBase;
            config.query.staticIndirect->capacity = capacity;
            config.window.emplace();
            config.window->width = 320;
            config.window->height = 180;
            config.window->visible = false;
            config.window->title =
                "AR heap gpu-driven " + runId + " " + std::to_string(GetCurrentProcessId());
            config.diagnostics.runId = runId;
            config.diagnostics.outputRoot = runRoot;
            return config;
        };

    if (!supportsHeap) {
        auto rejected =
            runtime::RuntimeSession::start(makeConfig(true, "heap-unsupported", true, UINT32_MAX));
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().endResult.resourcesReleased);
        CHECK(rejected.error().endResult.residualResources.empty());
        return;
    }

    const auto verifyEvents = [&](const std::string& runId, bool heapMode, bool expectRollback) {
        std::ifstream events(runRoot / runId / "events.jsonl");
        REQUIRE(events);
        bool sawValidationEnabled = false;
        bool sawHeapReady = false;
        bool sawSetReady = false;
        std::size_t envReady = 0;
        std::size_t envRollback = 0;
        for (std::string line; std::getline(events, line);) {
            const auto id = Json::parse(line).at("event_id").get<std::string>();
            CHECK(id != "rhi.validation.error");
            sawValidationEnabled |= (id == "rhi.validation.enabled");
            sawHeapReady |= (id == "rhi.binding.heap.ready");
            sawSetReady |= (id == "rhi.binding.set.ready");
            envReady += (id == "rhi.environment.ready");
            envRollback += (id == "rhi.environment.rollback");
        }
#ifdef _DEBUG
        CHECK(sawValidationEnabled);
#else
        (void)sawValidationEnabled;
#endif
        CHECK(sawHeapReady == heapMode);
        CHECK(sawSetReady == !heapMode);
        if (expectRollback) {
            CHECK(envReady == 2u);
            CHECK(envRollback == 1u);
        }
    };

    struct MainRun {
        rhi::FrameScreenshot constantInitial;
        rhi::FrameScreenshot turnedAway;
        rhi::FrameScreenshot restoredConstant;
        rhi::FrameScreenshot afterRollbackConstant;
        rhi::FrameScreenshot directional1;
        rhi::FrameScreenshot directional2;
        rhi::FrameScreenshot resized1;
        rhi::FrameScreenshot resized2;
    };

    const auto runMain = [&](bool heapMode, const std::string& runId) -> MainRun {
        const auto config = makeConfig(heapMode, runId, true, UINT32_MAX);
        auto session = runtime::RuntimeSession::start(config);
        INFO(describeStart(session));
        REQUIRE(session.has_value());
        REQUIRE((*session)->openStaticScene(mainScenePath));
        CHECK((*session)->staticSceneUploadCount() == 1);
        REQUIRE((*session)->replaceEnvironment(constantEnv, false));

        const std::wstring title(config.window->title.begin(), config.window->title.end());
        const HWND window = FindWindowW(nullptr, title.c_str());
        REQUIRE(window != nullptr);

        const auto capture = [&](rhi::FrameScreenshot& out) {
            auto status = (*session)->presentFrame(&out);
            INFO((status ? "" : status.error().message));
            REQUIRE(status.has_value());
            REQUIRE(*status == runtime::FrameStatus::Presented);
            REQUIRE(out.gbuffer.size() == 3);
            REQUIRE(out.gbuffer[0].hdrColor.has_value());
            REQUIRE(out.gbuffer[1].hdrColor.has_value());
            REQUIRE(out.gbuffer[2].hdrColor.has_value());
            REQUIRE(out.indirect.has_value());
        };

        MainRun out;
        capture(out.constantInitial);

        // Turn 180 deg away: zero visible objects, zero re-uploads.
        REQUIRE((*session)->setCamera(turnedAway));
        CHECK((*session)->staticSceneUploadCount() == 1);
        capture(out.turnedAway);

        // Restore initial camera: identical to initial capture.
        REQUIRE((*session)->setCamera(initialCamera));
        CHECK((*session)->staticSceneUploadCount() == 1);
        capture(out.restoredConstant);

        // Live environment rollback must preserve active Constant environment.
        const auto rolledBack = (*session)->replaceEnvironment(directionalEnv, true);
        REQUIRE_FALSE(rolledBack.has_value());
        capture(out.afterRollbackConstant);

        // Live environment replacement Constant -> Directional across both frame slots.
        REQUIRE((*session)->replaceEnvironment(directionalEnv, false));
        capture(out.directional1);
        capture(out.directional2);

        // Window Resize exercises external attachment slot safe-update protocol across both slots.
        REQUIRE(SetWindowPos(
            window, nullptr, 0, 0, 460, 280, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
        RECT client{};
        REQUIRE(GetClientRect(window, &client));
        capture(out.resized1);
        capture(out.resized2);
        CHECK(out.resized1.width == static_cast<std::uint32_t>(client.right));
        CHECK(out.resized1.height == static_cast<std::uint32_t>(client.bottom));
        CHECK(out.resized1.width != out.directional1.width);
        CHECK(out.resized1.height != out.directional1.height);
        CHECK(out.resized2.width == out.resized1.width);
        CHECK(out.resized2.height == out.resized1.height);
        CHECK(out.resized2.rgba == out.resized1.rgba);

        const auto closed = (*session)->close();
        REQUIRE(closed.result == core::ResultKind::Success);
        REQUIRE(closed.resourcesReleased);
        CHECK((*session)->staticSceneUploadCount() == 1);
        verifyEvents(runId, heapMode, true);
        return out;
    };

    const MainRun setMain = runMain(false, "set-main");
    const MainRun heapMain = runMain(true, "heap-main");

    // Verify scene & environment physics on baseline before asserting Set/Heap parity.
    const std::vector<std::uint64_t> expectedVisibleIds{1u, 2u, 3u};
    CHECK(setMain.constantInitial.indirect->visible == 3u);
    CHECK(setMain.constantInitial.indirect->drawnIds == expectedVisibleIds);
    CHECK(setMain.turnedAway.indirect->visible == 0u);
    CHECK(setMain.turnedAway.indirect->drawnIds.empty());
    CHECK(setMain.turnedAway.gbuffer[0].depth == 0.F);
    CHECK((*setMain.turnedAway.gbuffer[0].hdrColor)[0] == 0.F);
    CHECK(setMain.restoredConstant.rgba == setMain.constantInitial.rgba);
    CHECK(setMain.afterRollbackConstant.rgba == setMain.constantInitial.rgba);
    CHECK(setMain.directional2.rgba == setMain.directional1.rgba);
    // Constant vs. Directional must change metallic lower quad HDR color noticeably.
    const auto hdrDiff = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
    };
    CHECK(
        hdrDiff(
            *setMain.constantInitial.gbuffer[1].hdrColor,
            *setMain.directional1.gbuffer[1].hdrColor) > 0.02F);
    CHECK((*setMain.directional1.gbuffer[2].hdrColor)[0] == 0.F);
    CHECK((*setMain.directional1.gbuffer[2].hdrColor)[1] == 0.F);
    CHECK((*setMain.directional1.gbuffer[2].hdrColor)[2] == 0.F);

    const auto checkParity = [](const rhi::FrameScreenshot& expected,
                                const rhi::FrameScreenshot& actual) {
        CHECK(actual.width == expected.width);
        CHECK(actual.height == expected.height);
        CHECK(actual.rgba == expected.rgba);
        REQUIRE(actual.indirect.has_value());
        REQUIRE(expected.indirect.has_value());
        CHECK(actual.indirect->visible == expected.indirect->visible);
        CHECK(actual.indirect->drawnIds == expected.indirect->drawnIds);
        REQUIRE(actual.gbuffer.size() == expected.gbuffer.size());
        for (std::size_t i = 0; i < expected.gbuffer.size(); ++i) {
            const auto& lhs = expected.gbuffer[i];
            const auto& rhs = actual.gbuffer[i];
            CHECK(rhs.pixel == lhs.pixel);
            CHECK(rhs.depth == Catch::Approx(lhs.depth).margin(1e-5F));
            for (std::size_t c = 0; c < 4; ++c) {
                CHECK(rhs.baseColor[c] == Catch::Approx(lhs.baseColor[c]).margin(2.F / 255.F));
            }
            for (std::size_t c = 0; c < 3; ++c) {
                CHECK(rhs.normal[c] == Catch::Approx(lhs.normal[c]).margin(1e-4F));
                CHECK(rhs.viewPosition[c] == Catch::Approx(lhs.viewPosition[c]).margin(1e-3F));
            }
            for (std::size_t c = 0; c < 2; ++c) {
                CHECK(rhs.material[c] == Catch::Approx(lhs.material[c]).margin(2.F / 255.F));
            }
            REQUIRE(lhs.hdrColor.has_value());
            REQUIRE(rhs.hdrColor.has_value());
            for (std::size_t c = 0; c < 3; ++c) {
                CHECK((*rhs.hdrColor)[c] == Catch::Approx((*lhs.hdrColor)[c]).margin(2e-3F));
            }
        }
    };

    checkParity(setMain.constantInitial, heapMain.constantInitial);
    checkParity(setMain.turnedAway, heapMain.turnedAway);
    checkParity(setMain.restoredConstant, heapMain.restoredConstant);
    checkParity(setMain.afterRollbackConstant, heapMain.afterRollbackConstant);
    checkParity(setMain.directional1, heapMain.directional1);
    checkParity(setMain.directional2, heapMain.directional2);
    checkParity(setMain.resized1, heapMain.resized1);
    checkParity(setMain.resized2, heapMain.resized2);

    // Overflow scenario (visible = 4 > capacity = 2) on direct deferred lighting (no IBL):
    // validate Set and Heap independently per design.md without demanding identical subsets/pixels.
    const auto runOverflow = [&](bool heapMode, const std::string& runId) {
        const auto config = makeConfig(heapMode, runId, false, 2u);
        auto session = runtime::RuntimeSession::start(config);
        INFO(describeStart(session));
        REQUIRE(session.has_value());
        REQUIRE((*session)->openStaticScene(overflowScenePath));
        rhi::FrameScreenshot frame;
        auto status = (*session)->presentFrame(&frame);
        INFO((status ? "" : status.error().message));
        REQUIRE(status.has_value());
        REQUIRE(*status == runtime::FrameStatus::Presented);
        REQUIRE(frame.indirect.has_value());
        CHECK(frame.indirect->visible == 4u);
        REQUIRE(frame.indirect->drawnIds.size() == 2u);
        CHECK(
            std::ranges::adjacent_find(frame.indirect->drawnIds) == frame.indirect->drawnIds.end());
        for (const std::uint64_t id : frame.indirect->drawnIds) {
            CHECK(id >= 1u);
            CHECK(id <= 4u);
        }
        const auto closed = (*session)->close();
        REQUIRE(closed.result == core::ResultKind::Success);
        REQUIRE(closed.resourcesReleased);
        verifyEvents(runId, heapMode, false);
    };

    runOverflow(false, "set-overflow");
    runOverflow(true, "heap-overflow");
}
