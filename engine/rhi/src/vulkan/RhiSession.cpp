#include "rhi/RhiSession.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <volk.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "TestingHooks.h"
#include "VulkanResult.h"

namespace ar::rhi {

namespace {

constexpr const char* kModuleName = "rhi";
constexpr int kMaxEnumerationAttempts = 3;

// Process-wide facts, not per-session: a cleanup that retained resources leaves the loaded
// library and possibly the volk entry table behind, so reusing them from a new session would
// silently work on a polluted process. P0 runs one owner thread, so plain members are enough.
struct ProcessState {
    bool quarantined = false;
#ifdef AR_ENABLE_TEST_HOOKS
    std::size_t loadCalls = 0;
    std::size_t volkInitializeCalls = 0;
    std::size_t instanceCreateCalls = 0;
    std::size_t reportCallsAfterClose = 0;
#endif
};

ProcessState g_process;

core::Error makeError(
    core::ErrorCategory category,
    core::ErrorStage stage,
    std::string message,
    std::optional<core::ErrorOrigin> origin = std::nullopt) {
    return core::Error{category, stage, std::move(message), std::move(origin)};
}

std::optional<core::ErrorOrigin> vulkanOrigin(VkResult result) {
    return core::ErrorOrigin{"vulkan", static_cast<std::int64_t>(result)};
}

std::optional<core::ErrorOrigin> osOrigin() {
    return core::ErrorOrigin{"os", static_cast<std::int64_t>(::GetLastError())};
}

// The only project-internal origin code in P0: the process was quarantined by an earlier
// cleanup failure, so this refusal did not load anything.
constexpr const char* kInternalOriginDomain = "internal";
constexpr std::int64_t kQuarantinedProcessCode = 1;

core::ResultKind resultKindFor(core::ErrorCategory category) {
    switch (category) {
    case core::ErrorCategory::InvalidArguments:
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

VulkanResultInfo describe(VkResult result) {
    return VulkanResultInfo{static_cast<std::int32_t>(result), detail::vkResultName(result)};
}

// Every event is best-effort: an emit failure is sticky inside core and surfaces at finish,
// so the query keeps running and reports what it actually measured.
void emitEvent(
    core::DiagnosticSession& diagnostics,
    core::LogLevel level,
    std::string_view eventId,
    std::string message,
    std::optional<core::ErrorStage> stage = std::nullopt,
    std::optional<core::ErrorOrigin> origin = std::nullopt) {
    if (!diagnostics.enabled(level)) {
        return;
    }
    core::Event event;
    event.level = level;
    event.module = kModuleName;
    event.eventId = std::string(eventId);
    event.message = std::move(message);
    event.stage = stage;
    event.origin = std::move(origin);
    (void)diagnostics.emit(event);
}

std::string apiText(std::uint32_t apiVersion) {
    const std::uint32_t major = (apiVersion >> 22) & 0x7Fu;
    const std::uint32_t minor = (apiVersion >> 12) & 0x3FFu;
    const std::uint32_t patch = apiVersion & 0xFFFu;
    std::string text = std::to_string(major) + "." + std::to_string(minor);
    if (patch != 0) {
        text += "." + std::to_string(patch);
    }
    return text;
}

std::optional<core::Error> validateRequest(const QueryConfig& config) {
    const std::uint32_t version = config.requestedApi;
    const std::uint32_t major = (version >> 22) & 0x7Fu;
    const std::uint32_t minor = (version >> 12) & 0x3FFu;
    if (version == 0) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "requested_api must name a Vulkan version");
    }
    if (detail::apiVariant(version) != 0) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "requested_api variant must be 0");
    }
    if (major != 1) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "requested_api major version must be 1");
    }
    if (minor > 4) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "requested_api above 1.4 is not a known Vulkan version");
    }
    if (const auto* explicitPath = std::get_if<ExplicitLoaderPath>(&config.loader);
        explicitPath != nullptr && explicitPath->path.empty()) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "explicit loader path is empty");
    }
    return std::nullopt;
}

CapabilitySource sourceFor(const QueryConfig& config) {
    if (const auto* explicitPath = std::get_if<ExplicitLoaderPath>(&config.loader);
        explicitPath != nullptr) {
        return CapabilitySource{
            "fixture_loader", explicitPath->path.filename().string(), true};
    }
    return CapabilitySource{"system_loader", std::string{}, false};
}

bool hasExtension(const std::vector<ExtensionInfo>& extensions, ExtensionScope scope,
    std::string_view name) {
    return std::any_of(extensions.begin(), extensions.end(), [&](const ExtensionInfo& extension) {
        return extension.scope == scope && extension.name == name;
    });
}

} // namespace

struct RhiSession::Impl {
    explicit Impl(core::DiagnosticSession& session) : diagnostics(&session) {}

    core::DiagnosticSession* diagnostics = nullptr;
    CapabilityReport report;
    std::vector<core::Error> cleanupErrors;
    std::vector<core::ResidualResource> residualResources;

    QueryConfig config;
    HMODULE module = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    bool volkInitialized = false;
    bool closed = false;
    CloseResult closeResult;

    void noteCleanup(core::Error error) {
        report.errors.push_back(ReportError{error.stage, error.category, std::nullopt});
        cleanupErrors.push_back(std::move(error));
    }

    void retain(core::ResourceKind kind) {
        residualResources.push_back(core::ResidualResource{kind, core::ResourceState::Retained});
    }

    std::expected<void, core::Error> loadModule() {
#ifdef AR_ENABLE_TEST_HOOKS
        ++g_process.loadCalls;
#endif
        if (const auto* explicitPath = std::get_if<ExplicitLoaderPath>(&config.loader);
            explicitPath != nullptr) {
            module = ::LoadLibraryW(explicitPath->path.wstring().c_str());
        } else {
            module = ::LoadLibraryW(L"vulkan-1.dll");
        }
        if (module == nullptr) {
            const auto origin = osOrigin();
            return std::unexpected(makeError(
                core::ErrorCategory::LoaderUnavailable,
                core::ErrorStage::Loader,
                "the Vulkan loader could not be loaded",
                origin));
        }
        getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            ::GetProcAddress(module, "vkGetInstanceProcAddr"));
        if (getInstanceProcAddr == nullptr) {
            return std::unexpected(makeError(
                core::ErrorCategory::LoaderSymbolMissing,
                core::ErrorStage::Loader,
                "the loaded module has no vkGetInstanceProcAddr export"));
        }
        volkInitializeCustom(getInstanceProcAddr);
        volkInitialized = true;
#ifdef AR_ENABLE_TEST_HOOKS
        ++g_process.volkInitializeCalls;
#endif
        if (vkGetInstanceProcAddr == nullptr) {
            return std::unexpected(makeError(
                core::ErrorCategory::LoaderSymbolMissing,
                core::ErrorStage::Loader,
                "volk did not receive a usable vkGetInstanceProcAddr"));
        }
        return {};
    }

    std::expected<void, core::Error> queryLoaderApi() {
        std::uint32_t loaderVersion = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersion != nullptr) {
            const VkResult result = vkEnumerateInstanceVersion(&loaderVersion);
            if (result != VK_SUCCESS) {
                report.errors.push_back(
                    ReportError{core::ErrorStage::Loader,
                        core::ErrorCategory::QueryFailed, describe(result)});
                return std::unexpected(makeError(
                    core::ErrorCategory::QueryFailed,
                    core::ErrorStage::Loader,
                    "vkEnumerateInstanceVersion failed",
                    vulkanOrigin(result)));
            }
        }
        report.loaderApi = loaderVersion;
        report.apiVariants.loader = detail::apiVariant(loaderVersion);
        return {};
    }

    std::expected<void, core::Error> createInstance() {
        if (report.apiVariants.loader.has_value() && *report.apiVariants.loader != 0) {
            report.variantSupported = VariantSupport::False;
            return std::unexpected(makeError(
                core::ErrorCategory::ApiVariantUnsupported,
                core::ErrorStage::Instance,
                "the loader reports API variant "
                    + std::to_string(*report.apiVariants.loader)));
        }
        if (report.loaderApi.has_value() && config.requestedApi > *report.loaderApi) {
            return std::unexpected(makeError(
                core::ErrorCategory::ApiVersionUnsupported,
                core::ErrorStage::Instance,
                "the loader supports " + apiText(*report.loaderApi) + " but "
                    + apiText(config.requestedApi) + " was requested"));
        }
        if (vkCreateInstance == nullptr) {
            return std::unexpected(makeError(
                core::ErrorCategory::LoaderSymbolMissing,
                core::ErrorStage::Loader,
                "vkCreateInstance is not available from the loader"));
        }

        VkApplicationInfo applicationInfo{};
        applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        applicationInfo.pApplicationName = "AR Engine";
        applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        applicationInfo.pEngineName = "AR Engine";
        applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        applicationInfo.apiVersion = config.requestedApi;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &applicationInfo;

        report.instanceCreate.requested = config.requestedApi;
#ifdef AR_ENABLE_TEST_HOOKS
        ++g_process.instanceCreateCalls;
#endif
        const VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
        report.instanceCreate.vkResult = describe(result);
        if (result != VK_SUCCESS) {
            report.instanceCreate.status = InstanceCreateStatus::Failed;
            instance = VK_NULL_HANDLE;
            const bool incompatible = result == VK_ERROR_INCOMPATIBLE_DRIVER;
            return std::unexpected(makeError(
                incompatible ? core::ErrorCategory::RequestedApiUnsupported
                             : core::ErrorCategory::InitializationFailed,
                core::ErrorStage::Instance,
                incompatible ? "the loader rejected the requested API version"
                             : "vkCreateInstance failed",
                vulkanOrigin(result)));
        }
        report.instanceCreate.status = InstanceCreateStatus::Success;
        volkLoadInstance(instance);
        return {};
    }

    std::expected<void, core::Error> queryInstanceExtensions() {
        std::uint32_t count = 0;
        VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        if (result != VK_SUCCESS) {
            report.instanceExtensionQuery.status = QueryStatus::Failed;
            report.instanceExtensionQuery.vkResult = describe(result);
            return std::unexpected(makeError(
                core::ErrorCategory::QueryFailed,
                core::ErrorStage::Capability,
                "vkEnumerateInstanceExtensionProperties failed",
                vulkanOrigin(result)));
        }
        std::vector<VkExtensionProperties> properties(count);
        if (count > 0) {
            result = vkEnumerateInstanceExtensionProperties(nullptr, &count, properties.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                report.instanceExtensionQuery.status = QueryStatus::Failed;
                report.instanceExtensionQuery.vkResult = describe(result);
                return std::unexpected(makeError(
                    core::ErrorCategory::QueryFailed,
                    core::ErrorStage::Capability,
                    "vkEnumerateInstanceExtensionProperties failed",
                    vulkanOrigin(result)));
            }
            properties.resize(std::min<std::size_t>(properties.size(), count));
        }
        report.instanceExtensionQuery.status = QueryStatus::Success;
        for (const VkExtensionProperties& property : properties) {
            report.extensions.push_back(
                ExtensionInfo{ExtensionScope::Instance, property.extensionName, property.specVersion});
        }
        return {};
    }

    std::expected<std::vector<VkPhysicalDevice>, core::Error> enumerateDevices() {
        std::vector<VkPhysicalDevice> devices;
        for (int attempt = 0; attempt < kMaxEnumerationAttempts; ++attempt) {
            std::uint32_t count = 0;
            VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
            if (result != VK_SUCCESS) {
                return std::unexpected(makeError(
                    core::ErrorCategory::QueryFailed,
                    core::ErrorStage::Enumeration,
                    "vkEnumeratePhysicalDevices failed while counting devices",
                    vulkanOrigin(result)));
            }
            devices.resize(count);
            if (count == 0) {
                return devices;
            }
            result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
            if (result == VK_SUCCESS) {
                devices.resize(std::min<std::size_t>(devices.size(), count));
                return devices;
            }
            if (result != VK_INCOMPLETE) {
                return std::unexpected(makeError(
                    core::ErrorCategory::QueryFailed,
                    core::ErrorStage::Enumeration,
                    "vkEnumeratePhysicalDevices failed",
                    vulkanOrigin(result)));
            }
        }
        return std::unexpected(makeError(
            core::ErrorCategory::QueryFailed,
            core::ErrorStage::Enumeration,
            "vkEnumeratePhysicalDevices kept returning VK_INCOMPLETE after 3 attempts",
            vulkanOrigin(VK_INCOMPLETE)));
    }

    std::expected<VkPhysicalDevice, core::Error> selectDevice(
        const std::vector<VkPhysicalDevice>& devices) {
        if (devices.empty()) {
            return std::unexpected(makeError(
                core::ErrorCategory::DeviceNotFound,
                core::ErrorStage::Enumeration,
                "vkEnumeratePhysicalDevices returned no physical device"));
        }
        if (const auto* explicitIndex = std::get_if<ExplicitDeviceIndex>(&config.device);
            explicitIndex != nullptr) {
            if (explicitIndex->index >= devices.size()) {
                return std::unexpected(makeError(
                    core::ErrorCategory::InvalidArguments,
                    core::ErrorStage::Enumeration,
                    "explicit device index " + std::to_string(explicitIndex->index) + " is out of range ("
                        + std::to_string(devices.size()) + " device(s) enumerated)"));
            }
            return devices[explicitIndex->index];
        }
        return devices.front();
    }

    void queryDeviceFacts(VkPhysicalDevice device) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        report.deviceApi = properties.apiVersion;
        report.apiVariants.device = detail::apiVariant(properties.apiVersion);
        report.deviceName = properties.deviceName;
        report.driverVersion = properties.driverVersion;

        std::uint32_t count = 0;
        VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        if (result != VK_SUCCESS) {
            report.deviceExtensionQuery.status = QueryStatus::Failed;
            report.deviceExtensionQuery.vkResult = describe(result);
            report.errors.push_back(ReportError{
                core::ErrorStage::Capability, core::ErrorCategory::QueryFailed, describe(result)});
            return;
        }
        std::vector<VkExtensionProperties> propertiesList(count);
        if (count > 0) {
            result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, propertiesList.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                report.deviceExtensionQuery.status = QueryStatus::Failed;
                report.deviceExtensionQuery.vkResult = describe(result);
                report.errors.push_back(ReportError{
                    core::ErrorStage::Capability, core::ErrorCategory::QueryFailed, describe(result)});
                return;
            }
            propertiesList.resize(std::min<std::size_t>(propertiesList.size(), count));
        }
        report.deviceExtensionQuery.status = QueryStatus::Success;
        for (const VkExtensionProperties& property : propertiesList) {
            report.extensions.push_back(
                ExtensionInfo{ExtensionScope::Device, property.extensionName, property.specVersion});
        }
        queryDescriptorHeap(device);
    }

    void queryDescriptorHeap(VkPhysicalDevice device) {
        constexpr std::string_view kExtension = "VK_EXT_descriptor_heap";
        report.features["VK_EXT_descriptor_heap"] = FeatureInfo{};
        if (!hasExtension(report.extensions, ExtensionScope::Device, kExtension)) {
            report.descriptorHeap = DescriptorHeapStatus::Unsupported;
            report.features["VK_EXT_descriptor_heap"] =
                FeatureInfo{QueryStatus::Success, std::optional<bool>(false)};
            return;
        }
#if defined(VK_EXT_descriptor_heap)
        VkPhysicalDeviceDescriptorHeapFeaturesEXT heapFeatures{};
        heapFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &heapFeatures;
        vkGetPhysicalDeviceFeatures2(device, &features);
        const bool supported = heapFeatures.descriptorHeap == VK_TRUE;
        report.descriptorHeap =
            supported ? DescriptorHeapStatus::Supported : DescriptorHeapStatus::Unsupported;
        report.features["VK_EXT_descriptor_heap"] =
            FeatureInfo{QueryStatus::Success, std::optional<bool>(supported)};
#else
        // The headers predate the extension, so the feature cannot be read faithfully.
        report.descriptorHeap = DescriptorHeapStatus::NotQueried;
        report.features["VK_EXT_descriptor_heap"] = FeatureInfo{};
#endif
    }

    void judge() {
        // Resolve the variant question first: the version judgment below depends on it.
        if (report.variantSupported == VariantSupport::Unknown) {
            if (report.apiVariants.loader.has_value() && report.apiVariants.device.has_value()) {
                report.variantSupported = (*report.apiVariants.loader == 0
                                              && *report.apiVariants.device == 0)
                    ? VariantSupport::True
                    : VariantSupport::False;
            }
        }

        const bool loaderEnough = report.loaderApi.has_value() && *report.loaderApi >= kApiVersion1_4;
        const bool deviceEnough = report.deviceApi.has_value() && *report.deviceApi >= kApiVersion1_4;
        const bool requestedEnough = config.requestedApi >= kApiVersion1_4;
        const bool created = report.instanceCreate.status == InstanceCreateStatus::Success;
        report.judgments.apiVersionSupported =
            loaderEnough && deviceEnough && requestedEnough && created
            && report.variantSupported == VariantSupport::True;

        const bool requiredPresent = std::all_of(
            config.requiredDeviceExtensions.begin(),
            config.requiredDeviceExtensions.end(),
            [&](const std::string& name) {
                return hasExtension(report.extensions, ExtensionScope::Device, name);
            });
        report.judgments.requiredCapabilitiesSupported = requiredPresent;

        if (report.loaderApi.has_value() && report.deviceApi.has_value()) {
            report.effectiveApi = std::min({*report.loaderApi, *report.deviceApi, config.requestedApi});
        }
    }

    // Releases whatever was acquired, in reverse order, and records what could not be released.
    void shutdown() {
        if (instance != VK_NULL_HANDLE) {
            if (vkDestroyInstance != nullptr) {
                vkDestroyInstance(instance, nullptr);
            } else {
                retain(core::ResourceKind::Instance);
                noteCleanup(makeError(
                    core::ErrorCategory::CleanupFailed,
                    core::ErrorStage::Cleanup,
                    "vkDestroyInstance is unavailable, the instance was not released"));
            }
            instance = VK_NULL_HANDLE;
        }
        if (volkInitialized) {
            volkFinalize();
            volkInitialized = false;
        }
        if (module != nullptr) {
            if (!::FreeLibrary(module)) {
                retain(core::ResourceKind::OsModule);
                noteCleanup(makeError(
                    core::ErrorCategory::CleanupFailed,
                    core::ErrorStage::Cleanup,
                    "the loader module could not be unloaded",
                    osOrigin()));
            }
            module = nullptr;
        }
        getInstanceProcAddr = nullptr;
        if (!residualResources.empty()) {
            // Nothing about this state is retryable in P0: the leftover module or instance is
            // released only by process exit, and a second open would reuse a polluted process.
            g_process.quarantined = true;
        }
    }

    std::expected<void, core::Error> query() {
        report.capabilitySource = sourceFor(config);
        report.apiVariants.requested = detail::apiVariant(config.requestedApi);
        report.instanceCreate.requested = config.requestedApi;

        // Checked before any OS load, volk initialisation or instance creation: a process that
        // retained resources must not reuse the previously loaded entry table.
        if (g_process.quarantined) {
            report.productTargetSupported = false;
            report.result = core::ResultKind::InitializationFailed;
            report.errors.push_back(ReportError{
                core::ErrorStage::Cleanup, core::ErrorCategory::CleanupFailed, std::nullopt});
            return std::unexpected(makeError(
                core::ErrorCategory::CleanupFailed,
                core::ErrorStage::Cleanup,
                "the process is quarantined: a previous cleanup retained resources",
                core::ErrorOrigin{kInternalOriginDomain, kQuarantinedProcessCode}));
        }
        if (auto error = validateRequest(config)) {
            return std::unexpected(std::move(*error));
        }

        emitEvent(*diagnostics, core::LogLevel::Info, "rhi.query.start",
            "querying Vulkan capabilities for API " + apiText(config.requestedApi),
            core::ErrorStage::Validation);

        if (auto loaded = loadModule(); !loaded) {
            return loaded;
        }
        if (auto loaderApi = queryLoaderApi(); !loaderApi) {
            return loaderApi;
        }
        if (auto created = createInstance(); !created) {
            return created;
        }
        if (auto instanceExtensions = queryInstanceExtensions(); !instanceExtensions) {
            return instanceExtensions;
        }
        auto devices = enumerateDevices();
        if (!devices) {
            return std::unexpected(std::move(devices.error()));
        }
        auto device = selectDevice(*devices);
        if (!device) {
            return std::unexpected(std::move(device.error()));
        }
        queryDeviceFacts(*device);
        judge();

        if (report.deviceExtensionQuery.status == QueryStatus::Failed) {
            return std::unexpected(makeError(
                core::ErrorCategory::QueryFailed,
                core::ErrorStage::Capability,
                "the device extension list could not be queried"));
        }

        report.productTargetSupported =
            report.judgments.apiVersionSupported && report.judgments.requiredCapabilitiesSupported;
        report.result = report.judgments.apiVersionSupported ? core::ResultKind::Success
                                                             : core::ResultKind::Unsupported;

        emitEvent(*diagnostics, core::LogLevel::Info, "rhi.query.complete",
            "device '" + report.deviceName + "' reports API " + apiText(*report.deviceApi)
                + ", descriptor heap "
                + (report.descriptorHeap == DescriptorHeapStatus::Supported ? "supported"
                                                                            : "not supported"),
            core::ErrorStage::Capability);
        return {};
    }

    QueryFailure failure(core::Error error) {
        shutdown();
        report.productTargetSupported = false;
        report.result = resultKindFor(error.category);
        report.errors.push_back(
            ReportError{error.stage, error.category, error.origin.has_value() && error.origin->domain == "vulkan"
                    ? std::optional<VulkanResultInfo>(VulkanResultInfo{
                          static_cast<std::int32_t>(error.origin->code),
                          detail::vkResultName(static_cast<VkResult>(error.origin->code))})
                    : std::nullopt});
        emitEvent(*diagnostics, core::LogLevel::Error, "rhi.query.failed", error.message,
            error.stage, error.origin);
        QueryFailure result;
        result.error = std::move(error);
        result.cleanupErrors = cleanupErrors;
        result.report = report;
        return result;
    }

    CloseResult doClose() {
        emitEvent(*diagnostics, core::LogLevel::Info, "rhi.close.start",
            "releasing the Vulkan instance and loader", core::ErrorStage::Cleanup);
        shutdown();
        // Terminal for this session: a second close() returns the cached result, and a destructor
        // after an explicit close no longer retries the failed steps.
        closed = true;
        closeResult.closed = true;
        closeResult.errors = cleanupErrors;
        closeResult.residualResources = residualResources;
        closeResult.resourcesReleased = residualResources.empty();
        emitEvent(*diagnostics,
            closeResult.resourcesReleased ? core::LogLevel::Info : core::LogLevel::Error,
            "rhi.close.complete",
            closeResult.resourcesReleased ? "all Vulkan resources were released"
                                          : "some Vulkan resources were retained",
            core::ErrorStage::Cleanup);
        return closeResult;
    }
};

RhiSession::RhiSession(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

RhiSession::~RhiSession() {
    if (!m_impl || m_impl->closed) {
        return;
    }
    const CloseResult result = m_impl->doClose();
    if (!result.errors.empty()) {
        std::fputs("rhi: session destroyed without close; cleanup reported errors\n", stderr);
    }
}

std::expected<std::unique_ptr<RhiSession>, QueryFailure> RhiSession::open(
    const QueryConfig& config,
    core::DiagnosticSession& diagnostics) {
    auto impl = std::make_unique<Impl>(diagnostics);
    impl->config = config;
    if (auto queried = impl->query(); !queried) {
        return std::unexpected(impl->failure(std::move(queried.error())));
    }
    return std::unique_ptr<RhiSession>(new RhiSession(std::move(impl)));
}

const CapabilityReport& RhiSession::report() const noexcept {
    if (m_impl->closed) {
        // The contract allows report() only while open and not closed. The frozen snapshot is
        // kept alive instead of dangling so this misuse stays observable rather than undefined;
        // test builds count it so the precondition breach cannot pass unnoticed.
#ifdef AR_ENABLE_TEST_HOOKS
        ++g_process.reportCallsAfterClose;
#endif
    }
    return m_impl->report;
}

CloseResult RhiSession::close() {
    if (m_impl->closed) {
        return m_impl->closeResult;
    }
    return m_impl->doClose();
}

#ifdef AR_ENABLE_TEST_HOOKS

namespace detail {

bool isProcessQuarantined() noexcept {
    return g_process.quarantined;
}

void clearProcessQuarantine() noexcept {
    g_process.quarantined = false;
}

ProcessCounters processCounters() noexcept {
    return ProcessCounters{g_process.loadCalls, g_process.volkInitializeCalls,
        g_process.instanceCreateCalls, g_process.reportCallsAfterClose};
}

void resetProcessCounters() noexcept {
    g_process.loadCalls = 0;
    g_process.volkInitializeCalls = 0;
    g_process.instanceCreateCalls = 0;
    g_process.reportCallsAfterClose = 0;
}

} // namespace detail

#endif // AR_ENABLE_TEST_HOOKS

} // namespace ar::rhi
