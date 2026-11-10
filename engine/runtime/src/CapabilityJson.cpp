#include "CapabilityJson.h"

#include <cstdint>
#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "core/Error.h"
#include "rhi/Capabilities.h"

namespace ar::runtime::detail {
namespace {

using Json = nlohmann::ordered_json;

const char* modeName(QueryMode mode) noexcept {
    switch (mode) {
    case QueryMode::Inspect:
        return "inspect";
    case QueryMode::RequireProduct:
        return "require_product";
    case QueryMode::Development:
        return "development";
    }
    return "inspect";
}

const char* queryStatusName(rhi::QueryStatus status) noexcept {
    switch (status) {
    case rhi::QueryStatus::NotQueried:
        return "not_queried";
    case rhi::QueryStatus::Success:
        return "success";
    case rhi::QueryStatus::Failed:
        return "failed";
    }
    return "not_queried";
}

const char* instanceCreateName(rhi::InstanceCreateStatus status) noexcept {
    switch (status) {
    case rhi::InstanceCreateStatus::NotAttempted:
        return "not_attempted";
    case rhi::InstanceCreateStatus::Success:
        return "success";
    case rhi::InstanceCreateStatus::Failed:
        return "failed";
    }
    return "not_attempted";
}

const char* variantName(rhi::VariantSupport support) noexcept {
    switch (support) {
    case rhi::VariantSupport::False:
        return "false";
    case rhi::VariantSupport::True:
        return "true";
    case rhi::VariantSupport::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* descriptorHeapName(rhi::DescriptorHeapStatus status) noexcept {
    switch (status) {
    case rhi::DescriptorHeapStatus::NotQueried:
        return "not_queried";
    case rhi::DescriptorHeapStatus::QueryFailed:
        return "query_failed";
    case rhi::DescriptorHeapStatus::Unsupported:
        return "unsupported";
    case rhi::DescriptorHeapStatus::Supported:
        return "supported";
    }
    return "not_queried";
}

Json vkResultJson(const std::optional<rhi::VulkanResultInfo>& result) {
    if (!result.has_value()) {
        return nullptr;
    }
    Json value = Json::object();
    value["code"] = result->code;
    value["name"] = result->name;
    return value;
}

Json sourceJson(const core::SourceStamp& source) {
    Json value = Json::object();
    value["revision"] = source.revision.has_value() ? Json(*source.revision) : Json(nullptr);
    if (!source.dirty.has_value()) {
        value["dirty"] = "unknown";
    } else {
        value["dirty"] = *source.dirty;
    }
    return value;
}

Json extensionQueryJson(const rhi::ExtensionQuery& query) {
    Json value = Json::object();
    value["status"] = queryStatusName(query.status);
    value["vk_result"] = vkResultJson(query.vkResult);
    return value;
}

} // namespace

EncodedCapability encodeCapability(const rhi::CapabilityReport& report,
    QueryMode mode,
    const std::string& runId,
    const core::BuildInfo& build) {
    try {
        Json capability = Json::object();
        capability["schema_version"] = 1;
        capability["run_id"] = runId;
        capability["build_id"] = build.buildId;

        Json source = sourceJson(build.source);
        source["input_hash"] = build.inputHash;
        capability["source"] = std::move(source);
        capability["input_hash"] = build.inputHash;
        capability["configuration"] = build.configuration;
        capability["mode"] = modeName(mode);

        Json productTarget = Json::object();
        productTarget["variant"] = 0;
        productTarget["major"] = 1;
        productTarget["minor"] = 4;
        productTarget["patch"] = 0;
        capability["product_target"] = std::move(productTarget);

        Json profile = Json::object();
        profile["id"] = "p0-version-query";
        profile["required_extensions"] = Json::array();
        profile["required_features"] = Json::array();
        profile["scope"] = "query_only";
        capability["requirement_profile"] = std::move(profile);

        capability["loader_api"] = report.loaderApi.has_value() ? Json(*report.loaderApi)
                                                                : Json(nullptr);
        capability["device_api"] = report.deviceApi.has_value() ? Json(*report.deviceApi)
                                                                : Json(nullptr);

        Json variants = Json::object();
        variants["loader"] = report.apiVariants.loader.has_value()
            ? Json(*report.apiVariants.loader)
            : Json(nullptr);
        variants["requested"] = report.apiVariants.requested;
        variants["device"] = report.apiVariants.device.has_value()
            ? Json(*report.apiVariants.device)
            : Json(nullptr);
        capability["api_variants"] = std::move(variants);

        Json create = Json::object();
        create["requested"] = report.instanceCreate.requested;
        create["status"] = instanceCreateName(report.instanceCreate.status);
        create["vk_result"] = vkResultJson(report.instanceCreate.vkResult);
        capability["instance_create"] = std::move(create);

        Json statuses = Json::object();
        statuses["instance"] = extensionQueryJson(report.instanceExtensionQuery);
        statuses["device"] = extensionQueryJson(report.deviceExtensionQuery);
        capability["extensions_query_status"] = std::move(statuses);

        Json extensions = Json::array();
        for (const rhi::ExtensionInfo& extension : report.extensions) {
            Json entry = Json::object();
            entry["scope"] = extension.scope == rhi::ExtensionScope::Instance ? "instance"
                                                                              : "device";
            entry["name"] = extension.name;
            entry["spec_version"] = extension.specVersion;
            extensions.push_back(std::move(entry));
        }
        capability["extensions"] = std::move(extensions);

        Json features = Json::object();
        for (const auto& [name, feature] : report.features) {
            Json entry = Json::object();
            entry["value"] = feature.supported.has_value() ? Json(*feature.supported)
                                                           : Json(nullptr);
            entry["query_status"] = queryStatusName(feature.status);
            features[name] = std::move(entry);
        }
        capability["features"] = std::move(features);

        Json capabilitySource = Json::object();
        capabilitySource["kind"] = report.capabilitySource.kind;
        capabilitySource["id"] = report.capabilitySource.id.empty()
            ? Json(nullptr)
            : Json(report.capabilitySource.id);
        capabilitySource["simulated"] = report.capabilitySource.simulated;
        capability["capability_source"] = std::move(capabilitySource);

        capability["variant_supported"] = variantName(report.variantSupported);
        capability["effective_api"] = report.effectiveApi.has_value() ? Json(*report.effectiveApi)
                                                                      : Json(nullptr);

        Json judgments = Json::object();
        judgments["api_version_supported"] = report.judgments.apiVersionSupported;
        judgments["required_capabilities_supported"] =
            report.judgments.requiredCapabilitiesSupported;
        capability["judgments"] = std::move(judgments);

        capability["product_target_supported"] = report.productTargetSupported;
        capability["device_name"] = report.deviceName.empty() ? Json(nullptr)
                                                              : Json(report.deviceName);
        capability["driver_version"] = report.driverVersion.has_value()
            ? Json(*report.driverVersion)
            : Json(nullptr);
        Json descriptorHeap = Json::object();
        descriptorHeap["status"] = descriptorHeapName(report.descriptorHeap);
        capability["descriptor_heap"] = std::move(descriptorHeap);
        capability["renderer_runtime_validated"] = report.rendererRuntimeValidated;
        capability["result"] = core::toString(report.result);

        Json errors = Json::array();
        for (const rhi::ReportError& error : report.errors) {
            Json entry = Json::object();
            entry["stage"] = core::toString(error.stage);
            entry["category"] = core::toString(error.category);
            entry["vk_result"] = vkResultJson(error.vkResult);
            errors.push_back(std::move(entry));
        }
        capability["errors"] = std::move(errors);

        EncodedCapability encoded;
        encoded.json = capability.dump();
        return encoded;
    } catch (const std::exception& ex) {
        // Only an encoding failure reaches here; an empty object or a truncated text must never
        // stand in for a capability product.
        EncodedCapability encoded;
        encoded.error = core::Error{core::ErrorCategory::OutputFailed,
            core::ErrorStage::Summary,
            std::string("capability_json could not be encoded: ") + ex.what(),
            std::nullopt};
        return encoded;
    }
}

} // namespace ar::runtime::detail
