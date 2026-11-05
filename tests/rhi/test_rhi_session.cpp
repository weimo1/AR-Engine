#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <windows.h>

#include "TestingHooks.h"
#include "core/BuildInfo.h"
#include "core/Diagnostics.h"
#include "rhi/RhiSession.h"

namespace core = ar::core;
namespace rhi = ar::rhi;

namespace {

std::filesystem::path testOutputRoot() {
    char* buffer = nullptr;
    std::size_t size = 0;
    REQUIRE(_dupenv_s(&buffer, &size, "AR_TEST_OUTPUT_ROOT") == 0);
    REQUIRE(buffer != nullptr);
    std::unique_ptr<char, decltype(&std::free)> owned(buffer, &std::free);
    return std::filesystem::path(owned.get());
}

core::RunSummaryIdentity identity() {
    const core::BuildInfo& info = core::buildInfo();
    core::RunSummaryIdentity identity;
    identity.buildId = info.buildId;
    identity.inputHash = info.inputHash;
    identity.configuration = info.configuration;
    identity.source = info.source;
    return identity;
}

// Fresh exclusive run directory per case, as the production contract requires.
std::filesystem::path freshRunDirectory(const std::string& runId) {
    const std::filesystem::path directory = testOutputRoot() / "rhi" / runId;
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory.parent_path());
    return directory;
}

std::unique_ptr<core::DiagnosticSession> makeSession(
    const std::filesystem::path& directory,
    const std::string& runId) {
    core::DiagnosticConfig config;
    config.outputRoot = directory;
    config.runId = runId;
    auto created = core::DiagnosticSession::create(config, identity());
    REQUIRE(created.has_value());
    return std::move(*created);
}

void finishSession(core::DiagnosticSession& session, const std::string& runId) {
    core::RunSummaryInput summary;
    summary.runId = runId;
    summary.identity = identity();
    summary.primaryResult = core::ResultKind::Success;
    const core::EndResult result = session.finish(summary);
    CHECK(result.result == core::ResultKind::Success);
    CHECK(result.diagnosticsComplete);
}

std::filesystem::path systemDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(buffer, MAX_PATH);
    REQUIRE(length > 0);
    return std::filesystem::path(buffer);
}

} // namespace

TEST_CASE("an API variant request is rejected before any loader is touched",
    "[rhi-request-validation]") {
    const std::string runId = "rhi-request-validation";
    const std::filesystem::path directory = freshRunDirectory(runId);
    auto session = makeSession(directory, runId);

    rhi::QueryConfig config;
    // A missing DLL on purpose: reaching the loader would report loader_unavailable instead.
    config.loader = rhi::ExplicitLoaderPath{directory / "absent-loader.dll"};
    config.requestedApi = (1u << 29) | rhi::makeApiVersion(1, 0);

    auto opened = rhi::RhiSession::open(config, *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::InvalidArguments);
    CHECK(failure.error.stage == core::ErrorStage::Validation);
    CHECK(failure.report.result == core::ResultKind::InvalidArguments);
    CHECK(failure.report.instanceCreate.status == rhi::InstanceCreateStatus::NotAttempted);
    CHECK(failure.report.instanceCreate.requested == config.requestedApi);
    CHECK(failure.report.apiVariants.requested == static_cast<std::int32_t>(config.requestedApi >> 29));
    CHECK(failure.report.capabilitySource.kind == "fixture_loader");
    CHECK(failure.report.capabilitySource.simulated);
    CHECK(failure.report.capabilitySource.id ==
        std::get<rhi::ExplicitLoaderPath>(config.loader).path.filename().string());
    CHECK_FALSE(failure.report.loaderApi.has_value());
    CHECK_FALSE(failure.report.productTargetSupported);

    finishSession(*session, runId);
}

TEST_CASE("an empty explicit loader path is rejected", "[rhi-request-validation]") {
    const std::string runId = "rhi-empty-loader-path";
    const std::filesystem::path directory = freshRunDirectory(runId);
    auto session = makeSession(directory, runId);

    rhi::QueryConfig config;
    config.loader = rhi::ExplicitLoaderPath{};
    auto opened = rhi::RhiSession::open(config, *session);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().error.category == core::ErrorCategory::InvalidArguments);
    CHECK(opened.error().error.stage == core::ErrorStage::Validation);

    finishSession(*session, runId);
}

TEST_CASE("a missing loader file reports loader_unavailable", "[rhi-loader-unavailable]") {
    const std::string runId = "rhi-loader-unavailable";
    const std::filesystem::path directory = freshRunDirectory(runId);
    auto session = makeSession(directory, runId);

    rhi::QueryConfig config;
    const std::filesystem::path missing = directory / "absent-loader.dll";
    config.loader = rhi::ExplicitLoaderPath{missing};
    config.requestedApi = rhi::kApiVersion1_4;

    auto opened = rhi::RhiSession::open(config, *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::LoaderUnavailable);
    CHECK(failure.error.stage == core::ErrorStage::Loader);
    REQUIRE(failure.error.origin.has_value());
    CHECK(failure.error.origin->domain == "os");
    CHECK(failure.error.origin->code != 0);
    CHECK(failure.report.result == core::ResultKind::InitializationFailed);
    CHECK(failure.report.instanceCreate.status == rhi::InstanceCreateStatus::NotAttempted);
    CHECK(failure.report.capabilitySource.kind == "fixture_loader");
    CHECK(failure.report.capabilitySource.simulated);
    CHECK(failure.report.capabilitySource.id == "absent-loader.dll");
    CHECK_FALSE(failure.report.loaderApi.has_value());

    finishSession(*session, runId);
}

TEST_CASE("a module without vkGetInstanceProcAddr reports loader_symbol_missing",
    "[rhi-loader-symbol-missing]") {
    const std::string runId = "rhi-loader-symbol-missing";
    const std::filesystem::path directory = freshRunDirectory(runId);
    auto session = makeSession(directory, runId);

    rhi::QueryConfig config;
    // A real DLL that certainly lacks the Vulkan entry point.
    config.loader = rhi::ExplicitLoaderPath{systemDirectory() / "kernel32.dll"};

    auto opened = rhi::RhiSession::open(config, *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::LoaderSymbolMissing);
    CHECK(failure.error.stage == core::ErrorStage::Loader);
    CHECK(failure.report.instanceCreate.status == rhi::InstanceCreateStatus::NotAttempted);

    finishSession(*session, runId);
}

// Not part of the required set: this one depends on the machine's real loader and GPU.
TEST_CASE("the system loader can be queried", "[.][rhi-system]") {
    const std::string runId = "rhi-system";
    const std::filesystem::path directory = freshRunDirectory(runId);
    auto session = makeSession(directory, runId);

    rhi::QueryConfig config;
    config.loader = rhi::SystemLoader{};
    auto opened = rhi::RhiSession::open(config, *session);
    if (!opened.has_value()) {
        WARN("system query failed: " << opened.error().error.message);
        finishSession(*session, runId);
        return;
    }
    const rhi::CapabilityReport& report = opened.value()->report();
    WARN("device: " << report.deviceName << ", deviceApi present: " << report.deviceApi.has_value()
                    << ", descriptor heap: " << static_cast<int>(report.descriptorHeap)
                    << ", product supported: " << report.productTargetSupported);
    const rhi::CloseResult closed = opened.value()->close();
    CHECK(closed.closed);
    CHECK(closed.resourcesReleased);
    CHECK(closed.errors.empty());
    // Repeated close returns the cached result and does not touch the OS again.
    const rhi::CloseResult again = opened.value()->close();
    CHECK(again.closed);
    CHECK(again.residualResources.empty());

    finishSession(*session, runId);
}

namespace {

// The fixture DLL is emitted next to this executable, so the path stays valid in any checkout
// and never needs a narrow-encoded build absolute path.
std::filesystem::path fixtureModule() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    REQUIRE(length > 0);
    return std::filesystem::path(buffer).parent_path() / "ar_rhi_fixture.dll";
}

// The fixture reads this before answering any entry point, so it must be set before open().
rhi::QueryConfig fixtureConfig(const char* spec) {
    REQUIRE(_putenv_s("AR_RHI_FIXTURE_SPEC", spec) == 0);
    rhi::QueryConfig config;
    config.loader = rhi::ExplicitLoaderPath{fixtureModule()};
    config.requestedApi = rhi::kApiVersion1_4;
    return config;
}

// Vulkan result codes used by the fixture; the public rhi headers do not expose VkResult.
constexpr std::int64_t kIncomplete = 5;
constexpr std::int64_t kErrorInitializationFailed = -3;
constexpr std::int64_t kErrorFeatureNotPresent = -8;
constexpr std::int64_t kErrorIncompatibleDriver = -9;

} // namespace

TEST_CASE("a fixture without physical devices reports device_not_found", "[rhi-device-none]") {
    const std::string runId = "rhi-device-none";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("devices=0"), *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::DeviceNotFound);
    CHECK(failure.error.stage == core::ErrorStage::Enumeration);
    CHECK(failure.report.result == core::ResultKind::InitializationFailed);
    CHECK(failure.report.instanceCreate.status == rhi::InstanceCreateStatus::Success);
    CHECK(failure.report.loaderApi.has_value());
    CHECK_FALSE(failure.report.deviceApi.has_value());
    CHECK_FALSE(failure.report.productTargetSupported);
    CHECK(failure.cleanupErrors.empty());

    finishSession(*session, runId);
}

TEST_CASE("an explicit device index beyond the enumeration is invalid_arguments",
    "[rhi-device-index-out-of-range]") {
    const std::string runId = "rhi-device-index-out-of-range";
    auto session = makeSession(freshRunDirectory(runId), runId);

    rhi::QueryConfig config = fixtureConfig("devices=1");
    config.device = rhi::ExplicitDeviceIndex{1};
    auto opened = rhi::RhiSession::open(config, *session);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().error.category == core::ErrorCategory::InvalidArguments);
    CHECK(opened.error().error.stage == core::ErrorStage::Enumeration);
    CHECK(opened.error().report.result == core::ResultKind::InvalidArguments);
    // The instance was created and then released by the failure path.
    CHECK(opened.error().report.instanceCreate.status == rhi::InstanceCreateStatus::Success);
    CHECK(opened.error().cleanupErrors.empty());

    finishSession(*session, runId);
}

TEST_CASE("a loader below the requested version fails without creating an instance",
    "[rhi-api-version-unsupported]") {
    const std::string runId = "rhi-api-version-unsupported";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("loader=1.0"), *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::ApiVersionUnsupported);
    CHECK(failure.error.stage == core::ErrorStage::Instance);
    CHECK(failure.report.result == core::ResultKind::Unsupported);
    CHECK(failure.report.instanceCreate.status == rhi::InstanceCreateStatus::NotAttempted);
    REQUIRE(failure.report.loaderApi.has_value());
    CHECK(*failure.report.loaderApi == rhi::makeApiVersion(1, 0));

    finishSession(*session, runId);
}

TEST_CASE("a loader API variant is unsupported and leaves the instance uncreated",
    "[rhi-api-variant-unsupported]") {
    const std::string runId = "rhi-api-variant-unsupported";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("loader-variant=1"), *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::ApiVariantUnsupported);
    CHECK(failure.report.result == core::ResultKind::Unsupported);
    CHECK(failure.report.variantSupported == rhi::VariantSupport::False);
    CHECK(failure.report.instanceCreate.status == rhi::InstanceCreateStatus::NotAttempted);
    REQUIRE(failure.report.apiVariants.loader.has_value());
    CHECK(*failure.report.apiVariants.loader == 1);

    finishSession(*session, runId);
}

TEST_CASE("an incompatible driver answer is requested_api_unsupported",
    "[rhi-instance-incompatible]") {
    const std::string runId = "rhi-instance-incompatible";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("create=incompatible"), *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::RequestedApiUnsupported);
    CHECK(failure.report.result == core::ResultKind::Unsupported);
    CHECK(failure.report.instanceCreate.status == rhi::InstanceCreateStatus::Failed);
    REQUIRE(failure.report.instanceCreate.vkResult.has_value());
    CHECK(failure.report.instanceCreate.vkResult->code == kErrorIncompatibleDriver);
    REQUIRE(failure.error.origin.has_value());
    CHECK(failure.error.origin->domain == "vulkan");
    CHECK(failure.error.origin->code == kErrorIncompatibleDriver);

    finishSession(*session, runId);
}

TEST_CASE("a plain instance failure stays initialization_failed", "[rhi-instance-init-failed]") {
    const std::string runId = "rhi-instance-init-failed";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("create=init-failed"), *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::InitializationFailed);
    CHECK(failure.report.instanceCreate.status == rhi::InstanceCreateStatus::Failed);
    REQUIRE(failure.report.instanceCreate.vkResult.has_value());
    CHECK(failure.report.instanceCreate.vkResult->code == kErrorInitializationFailed);

    finishSession(*session, runId);
}

TEST_CASE("a failing instance extension query keeps the confirmed API facts",
    "[rhi-extension-query-failed]") {
    const std::string runId = "rhi-extension-query-failed";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("instance-ext=failed"), *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::QueryFailed);
    CHECK(failure.error.stage == core::ErrorStage::Capability);
    CHECK(failure.report.instanceExtensionQuery.status == rhi::QueryStatus::Failed);
    REQUIRE(failure.report.instanceExtensionQuery.vkResult.has_value());
    CHECK(failure.report.instanceExtensionQuery.vkResult->code == kErrorInitializationFailed);
    CHECK(failure.report.loaderApi.has_value());
    CHECK(failure.report.instanceCreate.status == rhi::InstanceCreateStatus::Success);
    CHECK_FALSE(failure.report.deviceApi.has_value());

    finishSession(*session, runId);
}

TEST_CASE("a device extension query failure is reported as query_failed",
    "[rhi-device-extension-query-failed]") {
    const std::string runId = "rhi-device-extension-query-failed";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("device-ext=failed"), *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::QueryFailed);
    CHECK(failure.error.stage == core::ErrorStage::Capability);
    CHECK(failure.report.deviceExtensionQuery.status == rhi::QueryStatus::Failed);
    REQUIRE(failure.report.deviceExtensionQuery.vkResult.has_value());
    CHECK(failure.report.deviceExtensionQuery.vkResult->code == kErrorFeatureNotPresent);
    // The API facts were already established before the extension query failed.
    CHECK(failure.report.deviceApi.has_value());
    CHECK_FALSE(failure.report.productTargetSupported);

    finishSession(*session, runId);
}

TEST_CASE("a permanently incomplete enumeration gives up with query_failed",
    "[rhi-enumeration-incomplete]") {
    const std::string runId = "rhi-enumeration-incomplete";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("devices=2;enumerate=incomplete"), *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::QueryFailed);
    CHECK(failure.error.stage == core::ErrorStage::Enumeration);
    REQUIRE(failure.error.origin.has_value());
    CHECK(failure.error.origin->code == kIncomplete);
    CHECK(failure.error.message.find("VK_INCOMPLETE") != std::string::npos);

    finishSession(*session, runId);
}

TEST_CASE("a device below the requested version is unsupported but not an error",
    "[rhi-device-version-insufficient]") {
    const std::string runId = "rhi-device-version-insufficient";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("device=1.3"), *session);
    REQUIRE(opened.has_value());
    const rhi::CapabilityReport& report = opened.value()->report();
    CHECK(report.result == core::ResultKind::Unsupported);
    CHECK_FALSE(report.judgments.apiVersionSupported);
    CHECK_FALSE(report.productTargetSupported);
    CHECK(report.descriptorHeap == rhi::DescriptorHeapStatus::Unsupported);
    REQUIRE(report.effectiveApi.has_value());
    CHECK(*report.effectiveApi == rhi::makeApiVersion(1, 3));
    const rhi::CloseResult closed = opened.value()->close();
    CHECK(closed.resourcesReleased);

    finishSession(*session, runId);
}

TEST_CASE("a descriptor heap capable fixture supports the product target",
    "[rhi-descriptor-heap-present]") {
    const std::string runId = "rhi-descriptor-heap-present";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("heap=present"), *session);
    REQUIRE(opened.has_value());
    const rhi::CapabilityReport& report = opened.value()->report();
    CHECK(report.result == core::ResultKind::Success);
    CHECK(report.descriptorHeap == rhi::DescriptorHeapStatus::Supported);
    const auto feature = report.features.find("VK_EXT_descriptor_heap");
    REQUIRE(feature != report.features.end());
    CHECK(feature->second.status == rhi::QueryStatus::Success);
    REQUIRE(feature->second.supported.has_value());
    CHECK(*feature->second.supported);
    CHECK(report.judgments.apiVersionSupported);
    CHECK(report.productTargetSupported);
    CHECK_FALSE(report.rendererRuntimeValidated);
    const rhi::CloseResult closed = opened.value()->close();
    CHECK(closed.closed);
    CHECK(closed.resourcesReleased);
    CHECK(closed.errors.empty());

    finishSession(*session, runId);
}

TEST_CASE("a retained instance is reported by close instead of being hidden",
    "[rhi-cleanup-instance-retained]") {
    const std::string runId = "rhi-cleanup-instance-retained";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("destroy=missing"), *session);
    REQUIRE(opened.has_value());
    const rhi::CloseResult closed = opened.value()->close();
    CHECK(closed.closed);
    CHECK_FALSE(closed.resourcesReleased);
    REQUIRE(closed.residualResources.size() == 1);
    CHECK(closed.residualResources.front().kind == core::ResourceKind::Instance);
    CHECK(closed.residualResources.front().state == core::ResourceState::Retained);
    REQUIRE(closed.errors.size() == 1);
    CHECK(closed.errors.front().category == core::ErrorCategory::CleanupFailed);
    CHECK(closed.errors.front().stage == core::ErrorStage::Cleanup);

    const rhi::CloseResult again = opened.value()->close();
    CHECK(again.closed);
    CHECK(again.errors.size() == closed.errors.size());
    CHECK(again.residualResources.size() == closed.residualResources.size());

    // This case quarantines the process by design; clear it so a whole-binary run continues
    // from a clean state instead of inheriting the retained instance.
    CHECK(rhi::detail::isProcessQuarantined());
    rhi::detail::clearProcessQuarantine();

    finishSession(*session, runId);
}

TEST_CASE("close is idempotent and keeps a single cleanup record", "[rhi-repeat-close]") {
    const std::string runId = "rhi-repeat-close";
    auto session = makeSession(freshRunDirectory(runId), runId);

    auto opened = rhi::RhiSession::open(fixtureConfig("devices=1"), *session);
    REQUIRE(opened.has_value());
    const rhi::CloseResult first = opened.value()->close();
    const rhi::CloseResult second = opened.value()->close();
    CHECK(first.closed);
    CHECK(second.closed);
    CHECK(first.resourcesReleased);
    CHECK(second.resourcesReleased);
    CHECK(first.errors.empty());
    CHECK(second.errors.empty());
    CHECK(first.residualResources.empty());
    CHECK(second.residualResources.empty());

    // Destroy the RHI owner while diagnostics are still open, so duplicate cleanup is observable.
    opened.value().reset();
    finishSession(*session, runId);
    std::ifstream events(testOutputRoot() / "rhi" / runId / runId / "events.jsonl");
    REQUIRE(events.is_open());
    std::size_t closeStarts = 0;
    for (std::string line; std::getline(events, line);) {
        if (line.find("rhi.close.start") != std::string::npos) {
            ++closeStarts;
        }
    }
    CHECK(closeStarts == 1);
}

TEST_CASE("report() stays usable only while the session is open", "[rhi-report-after-close]") {
    const std::string runId = "rhi-report-after-close";
    auto session = makeSession(freshRunDirectory(runId), runId);
    rhi::detail::clearProcessQuarantine();
    rhi::detail::resetProcessCounters();

    auto opened = rhi::RhiSession::open(fixtureConfig("devices=1"), *session);
    REQUIRE(opened.has_value());

    // The supported pattern: copy the snapshot before close, then keep using the copy.
    const rhi::CapabilityReport snapshot = opened.value()->report();
    CHECK(rhi::detail::processCounters().reportCallsAfterClose == 0);

    const rhi::CloseResult closed = opened.value()->close();
    CHECK(closed.closed);
    CHECK(snapshot.deviceName == opened.value()->report().deviceName);

    // Misuse is counted instead of dereferencing a destroyed snapshot, so a precondition breach
    // cannot pass silently and no test relies on undefined behaviour.
    CHECK(rhi::detail::processCounters().reportCallsAfterClose == 1);

    finishSession(*session, runId);
}

namespace {

// Runs one child process of this same executable, filtered to the hidden quarantine case, so the
// process-wide flag starts clean and cannot leak into the other cases.
bool runQuarantineChild() {
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    REQUIRE(length > 0);

    std::wstring commandLine = L"\"";
    commandLine += modulePath;
    commandLine += L"\" \"[.][rhi-quarantine-child]\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL started = ::CreateProcessW(modulePath, commandLine.data(), nullptr, nullptr, FALSE,
        0, nullptr, nullptr, &startup, &process);
    REQUIRE(started != FALSE);
    const DWORD wait = ::WaitForSingleObject(process.hProcess, 120000);
    INFO("quarantine child wait result: " << wait << ", timeout=" << (wait == WAIT_TIMEOUT));
    CHECK(wait == WAIT_OBJECT_0);
    if (wait != WAIT_OBJECT_0) {
        // Only terminate the child owned by this test; never leave it running after a timeout.
        CHECK(::TerminateProcess(process.hProcess, 1) != FALSE);
        CHECK(::WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        return false;
    }
    DWORD exitCode = 1;
    CHECK(::GetExitCodeProcess(process.hProcess, &exitCode) != FALSE);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return exitCode == 0;
}

} // namespace

TEST_CASE("a quarantined process refuses to open again", "[rhi-quarantine-reopen]") {
    CHECK(runQuarantineChild());
}

TEST_CASE("the quarantine child scenarios run in a dedicated process",
    "[.][rhi-quarantine-child]") {
    rhi::detail::clearProcessQuarantine();
    rhi::detail::resetProcessCounters();

    {
        const std::string runId = "rhi-quarantine-plant";
        auto session = makeSession(freshRunDirectory(runId), runId);
        auto opened = rhi::RhiSession::open(fixtureConfig("destroy=missing"), *session);
        REQUIRE(opened.has_value());
        const rhi::CloseResult closed = opened.value()->close();
        CHECK_FALSE(closed.resourcesReleased);
        finishSession(*session, runId);
    }
    REQUIRE(rhi::detail::isProcessQuarantined());

    // The refusal is decided before any OS load, so nothing of this attempt is counted.
    rhi::detail::resetProcessCounters();
    const std::string runId = "rhi-quarantine-reopen";
    auto session = makeSession(freshRunDirectory(runId), runId);
    const rhi::QueryConfig config = fixtureConfig("devices=1");
    auto opened = rhi::RhiSession::open(config, *session);
    REQUIRE_FALSE(opened.has_value());
    const rhi::QueryFailure& failure = opened.error();
    CHECK(failure.error.category == core::ErrorCategory::CleanupFailed);
    CHECK(failure.error.stage == core::ErrorStage::Cleanup);
    REQUIRE(failure.error.origin.has_value());
    CHECK(failure.error.origin->domain == "internal");
    CHECK(failure.error.origin->code == 1);
    CHECK(failure.report.result == core::ResultKind::InitializationFailed);
    CHECK(failure.report.instanceCreate.status == rhi::InstanceCreateStatus::NotAttempted);
    CHECK(failure.report.instanceCreate.requested == config.requestedApi);
    CHECK(failure.report.apiVariants.requested == static_cast<std::int32_t>(config.requestedApi >> 29));
    CHECK(failure.report.capabilitySource.kind == "fixture_loader");
    CHECK(failure.report.capabilitySource.simulated);
    CHECK(failure.report.capabilitySource.id ==
        std::get<rhi::ExplicitLoaderPath>(config.loader).path.filename().string());
    CHECK_FALSE(failure.report.loaderApi.has_value());
    CHECK_FALSE(failure.report.deviceApi.has_value());
    CHECK_FALSE(failure.report.productTargetSupported);
    CHECK(failure.cleanupErrors.empty());

    const rhi::detail::ProcessCounters counters = rhi::detail::processCounters();
    CHECK(counters.loadCalls == 0);
    CHECK(counters.volkInitializeCalls == 0);
    CHECK(counters.instanceCreateCalls == 0);

    // A finished diagnostic run does not lift the quarantine.
    finishSession(*session, runId);
    CHECK(rhi::detail::isProcessQuarantined());
}
