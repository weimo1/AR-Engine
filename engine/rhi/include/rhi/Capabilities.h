#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/Error.h"

namespace ar::rhi {

// Raw Vulkan result kept next to its symbolic name. rhi owns the name text so reports stay
// readable without exposing Vulkan headers through the public interface.
struct VulkanResultInfo {
    std::int32_t code = 0;
    std::string name;
};

enum class QueryStatus {
    NotQueried,
    Success,
    Failed,
};

enum class InstanceCreateStatus {
    NotAttempted,
    Success,
    Failed,
};

// Three-valued on purpose: an unreadable variant field must not be reported as supported.
enum class VariantSupport {
    False,
    True,
    Unknown,
};

enum class DescriptorHeapStatus {
    NotQueried,
    QueryFailed,
    Unsupported,
    Supported,
};

enum class ExtensionScope {
    Instance,
    Device,
};

struct ExtensionInfo {
    ExtensionScope scope = ExtensionScope::Instance;
    std::string name;
    std::uint32_t specVersion = 0;
};

struct FeatureInfo {
    QueryStatus status = QueryStatus::NotQueried;
    std::optional<bool> supported;
};

struct ExtensionQuery {
    QueryStatus status = QueryStatus::NotQueried;
    std::optional<VulkanResultInfo> vkResult;
};

// Where the capability numbers came from: "system_loader", "fixture_loader" or
// "fixture_vulkan_api". A simulated source always carries a stable id.
struct CapabilitySource {
    std::string kind;
    std::string id;
    bool simulated = false;
};

struct Judgments {
    bool apiVersionSupported = false;
    bool requiredCapabilitiesSupported = false;
};

struct ReportError {
    core::ErrorStage stage = core::ErrorStage::Validation;
    core::ErrorCategory category = core::ErrorCategory::InternalError;
    std::optional<VulkanResultInfo> vkResult;
};

// The variant bits of the three API versions. requested is always known because it is an input.
struct ApiVariants {
    std::optional<std::int32_t> loader;
    std::int32_t requested = 0;
    std::optional<std::int32_t> device;
};

struct InstanceCreate {
    std::uint32_t requested = 0;
    InstanceCreateStatus status = InstanceCreateStatus::NotAttempted;
    std::optional<VulkanResultInfo> vkResult;
};

// Owned query facts. Mirrors the D1 field names; the run summary encoder adds mode and identity.
struct CapabilityReport {
    std::optional<std::uint32_t> loaderApi;
    std::optional<std::uint32_t> deviceApi;
    ApiVariants apiVariants;
    InstanceCreate instanceCreate;
    ExtensionQuery instanceExtensionQuery;
    ExtensionQuery deviceExtensionQuery;
    std::vector<ExtensionInfo> extensions;
    std::map<std::string, FeatureInfo> features;
    CapabilitySource capabilitySource;
    VariantSupport variantSupported = VariantSupport::Unknown;
    std::optional<std::uint32_t> effectiveApi;
    Judgments judgments;
    bool productTargetSupported = false;
    std::string deviceName;
    std::optional<std::uint32_t> driverVersion;
    DescriptorHeapStatus descriptorHeap = DescriptorHeapStatus::NotQueried;
    // P0 never runs a renderer, so this stays false for every report.
    bool rendererRuntimeValidated = false;
    core::ResultKind result = core::ResultKind::Success;
    std::vector<ReportError> errors;
};

} // namespace ar::rhi
