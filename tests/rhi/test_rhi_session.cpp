#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include <windows.h>

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
