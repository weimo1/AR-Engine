#include "rhi/RhiSession.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <volk.h>
#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Presentation.h"
#include "TestingHooks.h"
#include "VulkanResult.h"
#include "resources/DeviceResources.h"

namespace ar::rhi {

namespace {

constexpr const char* kModuleName = "rhi";
constexpr int kMaxEnumerationAttempts = 3;

// Process-wide facts, not per-session: a cleanup that retained resources leaves the loaded
// library and possibly the volk entry table behind, so reusing them from a new session would
// silently work on a polluted process. P0 runs one owner thread, so plain members are enough.
struct ProcessState {
    bool active = false;
    bool quarantined = false;
#ifdef AR_ENABLE_TEST_HOOKS
    std::size_t loadCalls = 0;
    std::size_t volkInitializeCalls = 0;
    std::size_t instanceCreateCalls = 0;
    std::size_t destroyInstanceCalls = 0;
    std::size_t volkFinalizeCalls = 0;
    std::size_t unloadCalls = 0;
    std::size_t reportCallsAfterClose = 0;
    std::size_t quarantineChecks = 0;
    // Release steps in execution order, as defined in TestingHooks.h.
    std::vector<int> shutdownOrder;
    // Injected once by runtime-cleanup-failure: the OS refuses to unload an already destroyed
    // instance's module. The module stays loaded until process exit, which is what the assertion
    // about a retained OS module describes.
    bool failUnloadOnce = false;
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
    case core::ErrorCategory::InvalidState:
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

VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* userData) noexcept {
    const bool error = severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    try {
        emitEvent(
            *static_cast<core::DiagnosticSession*>(userData),
            error ? core::LogLevel::Error : core::LogLevel::Warning,
            error ? "rhi.validation.error" : "rhi.validation.warning",
            data->pMessage);
    } catch (...) {
        // The C callback must not unwind through Vulkan.
        std::fputs("rhi: could not forward a Vulkan validation message\n", stderr);
    }
    return VK_FALSE;
}

constexpr bool validationBuild() noexcept {
#ifdef _DEBUG
    return true;
#else
    return false;
#endif
}

constexpr std::uint32_t compatibilityVersion(std::uint32_t version) noexcept {
    return VK_MAKE_API_VERSION(
        VK_API_VERSION_VARIANT(version),
        VK_API_VERSION_MAJOR(version),
        VK_API_VERSION_MINOR(version),
        0);
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
    if (config.createGraphicsDevice && version < VK_API_VERSION_1_3) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "a graphics device requires a Vulkan API request of at least 1.3");
    }
    if (config.enableDescriptorHeap &&
        (!config.createGraphicsDevice || version < VK_API_VERSION_1_4)) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "Descriptor Heap requires a Vulkan 1.4 graphics device request");
    }
    if (config.triangleShaders &&
        (!config.presentation || config.triangleShaders->vertex.size() < 5 ||
         config.triangleShaders->fragment.size() < 5 ||
         config.triangleShaders->vertex.front() != 0x07230203u ||
         config.triangleShaders->fragment.front() != 0x07230203u)) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "triangle shaders require presentation and valid SPIR-V headers");
    }
    if (config.enableScreenshot && !config.presentation) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "screenshots require presentation");
    }
    if (config.presentation &&
        (!config.createGraphicsDevice || !config.presentation->windowHandle ||
         config.presentation->width == 0 || config.presentation->height == 0)) {
        return makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "presentation requires a graphics device and a nonempty window");
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
        return CapabilitySource{"fixture_loader", explicitPath->path.filename().string(), true};
    }
    return CapabilitySource{"system_loader", std::string{}, false};
}

bool hasExtension(
    const std::vector<ExtensionInfo>& extensions, ExtensionScope scope, std::string_view name) {
    return std::any_of(extensions.begin(), extensions.end(), [&](const ExtensionInfo& extension) {
        return extension.scope == scope && extension.name == name;
    });
}

struct QueueFamilies {
    std::uint32_t graphics;
    std::uint32_t present;
};

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
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice graphicsDevice = VK_NULL_HANDLE;
    VkPhysicalDevice graphicsPhysical = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    std::optional<detail::Presentation> presenter;
    std::optional<detail::DeviceResources> resources;
    std::optional<GraphicsDeviceInfo> graphicsInfo;
    bool ownsProcess = false;
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
                    ReportError{
                        core::ErrorStage::Loader,
                        core::ErrorCategory::QueryFailed,
                        describe(result)});
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
                "the loader reports API variant " + std::to_string(*report.apiVariants.loader)));
        }
        if (report.loaderApi.has_value() &&
            compatibilityVersion(config.requestedApi) > compatibilityVersion(*report.loaderApi)) {
            return std::unexpected(makeError(
                core::ErrorCategory::ApiVersionUnsupported,
                core::ErrorStage::Instance,
                "the loader supports " + apiText(*report.loaderApi) + " but " +
                    apiText(config.requestedApi) + " was requested"));
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

        constexpr const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        std::vector<const char*> extensions;
        if (config.presentation) {
            for (const auto& name : config.presentation->instanceExtensions) {
                extensions.push_back(name.c_str());
            }
            extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            extensions.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        }
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{};
        const bool enableValidation = config.createGraphicsDevice && validationBuild();
        if (enableValidation) {
            debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugInfo.pfnUserCallback = validationCallback;
            debugInfo.pUserData = diagnostics;
            createInfo.pNext = &debugInfo;
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = &validationLayer;
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        std::ranges::sort(extensions, {}, [](const char* name) { return std::string_view(name); });
        const auto duplicates = std::ranges::unique(
            extensions, {}, [](const char* name) { return std::string_view(name); });
        extensions.erase(duplicates.begin(), duplicates.end());
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

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
                incompatible
                    ? "the loader rejected the requested API version"
                    : (enableValidation && result == VK_ERROR_LAYER_NOT_PRESENT
                           ? "VK_LAYER_KHRONOS_validation is required for Debug device creation"
                       : config.presentation && result == VK_ERROR_EXTENSION_NOT_PRESENT
                           ? "a required Win32 presentation or diagnostic instance extension is "
                             "unavailable"
                       : enableValidation && result == VK_ERROR_EXTENSION_NOT_PRESENT
                           ? "VK_EXT_debug_utils is required for Debug device creation"
                           : "vkCreateInstance failed"),
                vulkanOrigin(result)));
        }
        report.instanceCreate.status = InstanceCreateStatus::Success;
        volkLoadInstance(instance);
        if (enableValidation) {
            if (vkCreateDebugUtilsMessengerEXT == nullptr ||
                vkDestroyDebugUtilsMessengerEXT == nullptr) {
                return std::unexpected(makeError(
                    core::ErrorCategory::LoaderSymbolMissing,
                    core::ErrorStage::Instance,
                    "Vulkan debug messenger entry points are unavailable"));
            }
            const VkResult debugResult =
                vkCreateDebugUtilsMessengerEXT(instance, &debugInfo, nullptr, &debugMessenger);
            if (debugResult != VK_SUCCESS) {
                debugMessenger = VK_NULL_HANDLE;
                return std::unexpected(makeError(
                    core::ErrorCategory::InitializationFailed,
                    core::ErrorStage::Instance,
                    "vkCreateDebugUtilsMessengerEXT failed",
                    vulkanOrigin(debugResult)));
            }
            if (config.presentation) {
                emitEvent(
                    *diagnostics,
                    core::LogLevel::Info,
                    "rhi.validation.enabled",
                    "Vulkan Validation messenger enabled");
            }
        }
        return {};
    }

    std::expected<void, core::Error> createSurface() {
        const auto createWin32Surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
            getInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
        if (!createWin32Surface || !vkDestroySurfaceKHR || !vkGetPhysicalDeviceSurfaceSupportKHR) {
            return std::unexpected(makeError(
                core::ErrorCategory::LoaderSymbolMissing,
                core::ErrorStage::Instance,
                "Win32 surface entry points are unavailable"));
        }
        const auto window = static_cast<HWND>(config.presentation->windowHandle);
        VkWin32SurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hwnd = window;
        info.hinstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
        if (!info.hinstance) {
            return std::unexpected(makeError(
                core::ErrorCategory::InitializationFailed,
                core::ErrorStage::Instance,
                "could not obtain the window's HINSTANCE",
                osOrigin()));
        }
        const VkResult result = createWin32Surface(instance, &info, nullptr, &surface);
        if (result != VK_SUCCESS) {
            surface = VK_NULL_HANDLE;
            return std::unexpected(makeError(
                core::ErrorCategory::InitializationFailed,
                core::ErrorStage::Instance,
                "vkCreateWin32SurfaceKHR failed",
                vulkanOrigin(result)));
        }
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
                ExtensionInfo{
                    ExtensionScope::Instance, property.extensionName, property.specVersion});
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

    std::expected<VkPhysicalDevice, core::Error>
    selectDevice(const std::vector<VkPhysicalDevice>& devices) {
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
                    "explicit device index " + std::to_string(explicitIndex->index) +
                        " is out of range (" + std::to_string(devices.size()) +
                        " device(s) enumerated)"));
            }
            return devices[explicitIndex->index];
        }
        return devices.front();
    }

    std::expected<void, core::Error> checkPresentationSupport(VkPhysicalDevice physical) {
        std::uint32_t count = 0;
        VkResult result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
        if (result == VK_SUCCESS && count > 0) {
            std::vector<VkExtensionProperties> extensions(count);
            result =
                vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
            if (result == VK_SUCCESS) {
                extensions.resize(count);
                for (const char* name :
                     {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                      VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME}) {
                    if (std::ranges::none_of(extensions, [name](const auto& extension) {
                            return std::string_view(extension.extensionName) == name;
                        })) {
                        return std::unexpected(makeError(
                            core::ErrorCategory::QueryFailed,
                            core::ErrorStage::Capability,
                            std::string("presentation requires ") + name));
                    }
                }
                VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance{};
                maintenance.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
                VkPhysicalDeviceFeatures2 features{};
                features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                features.pNext = &maintenance;
                vkGetPhysicalDeviceFeatures2(physical, &features);
                if (maintenance.swapchainMaintenance1 == VK_TRUE) {
                    return {};
                }
                return std::unexpected(makeError(
                    core::ErrorCategory::QueryFailed,
                    core::ErrorStage::Capability,
                    "the device cannot enable swapchainMaintenance1 presentation fences"));
            }
        }
        return std::unexpected(makeError(
            core::ErrorCategory::QueryFailed,
            core::ErrorStage::Capability,
            "could not enumerate device extensions for presentation",
            vulkanOrigin(result)));
    }

    std::expected<QueueFamilies, core::Error> graphicsFamilies(VkPhysicalDevice physical) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (detail::apiVariant(properties.apiVersion) != 0 ||
            compatibilityVersion(properties.apiVersion) <
                compatibilityVersion(config.requestedApi)) {
            return std::unexpected(makeError(
                core::ErrorCategory::ApiVersionUnsupported,
                core::ErrorStage::Capability,
                "the physical device does not support requested Vulkan API " +
                    apiText(config.requestedApi)));
        }
        VkPhysicalDeviceVulkan13Features core13{};
        core13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &core13;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        if (core13.dynamicRendering != VK_TRUE || core13.synchronization2 != VK_TRUE) {
            const char* missing =
                core13.dynamicRendering != VK_TRUE ? "dynamicRendering" : "synchronization2";
            return std::unexpected(makeError(
                core::ErrorCategory::QueryFailed,
                core::ErrorStage::Capability,
                std::string("missing required Vulkan 1.3 feature: ") + missing));
        }
        if (surface != VK_NULL_HANDLE) {
            if (auto support = checkPresentationSupport(physical); !support) {
                return std::unexpected(std::move(support.error()));
            }
        }

        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        if (!queues.empty()) {
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        }
        std::optional<std::uint32_t> graphics;
        std::optional<std::uint32_t> present;
        for (std::uint32_t family = 0; family < count; ++family) {
            if (queues[family].queueCount == 0) {
                continue;
            }
            const bool canDraw = (queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            if (canDraw && !graphics) {
                graphics = family;
            }
            if (surface == VK_NULL_HANDLE) {
                if (canDraw) {
                    return QueueFamilies{family, family};
                }
                continue;
            }
            VkBool32 canPresent = VK_FALSE;
            const VkResult result =
                vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &canPresent);
            if (result != VK_SUCCESS) {
                return std::unexpected(makeError(
                    core::ErrorCategory::QueryFailed,
                    core::ErrorStage::Capability,
                    "vkGetPhysicalDeviceSurfaceSupportKHR failed",
                    vulkanOrigin(result)));
            }
            if (canPresent == VK_TRUE) {
                if (canDraw) {
                    return QueueFamilies{family, family};
                }
                if (!present) {
                    present = family;
                }
            }
        }
        if (graphics && present) {
            return QueueFamilies{*graphics, *present};
        }
        return std::unexpected(makeError(
            core::ErrorCategory::QueryFailed,
            core::ErrorStage::Capability,
            graphics ? "the physical device exposes no queue for this surface"
                     : "the physical device exposes no graphics queue"));
    }

    std::expected<QueueFamilies, core::Error>
    selectGraphicsDevice(const std::vector<VkPhysicalDevice>& devices, VkPhysicalDevice& selected) {
        if (vkGetPhysicalDeviceProperties == nullptr || vkGetPhysicalDeviceFeatures2 == nullptr ||
            vkGetPhysicalDeviceQueueFamilyProperties == nullptr ||
            (surface != VK_NULL_HANDLE && vkEnumerateDeviceExtensionProperties == nullptr)) {
            return std::unexpected(makeError(
                core::ErrorCategory::LoaderSymbolMissing,
                core::ErrorStage::Capability,
                "Vulkan graphics device discovery entry points are unavailable"));
        }
        auto family = graphicsFamilies(selected);
        if (family || std::holds_alternative<ExplicitDeviceIndex>(config.device)) {
            return family;
        }
        for (std::size_t index = 1; index < devices.size(); ++index) {
            if (auto alternative = graphicsFamilies(devices[index]); alternative) {
                selected = devices[index];
                return alternative;
            }
        }
        return family;
    }

    void queryVulkan14Features(VkPhysicalDevice physical) {
        VkPhysicalDeviceVulkan14Features core14{};
        core14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &core14;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        report.features["maintenance5"] =
            FeatureInfo{QueryStatus::Success, core14.maintenance5 == VK_TRUE};
        report.features["maintenance6"] =
            FeatureInfo{QueryStatus::Success, core14.maintenance6 == VK_TRUE};
        report.features["dynamicRenderingLocalRead"] =
            FeatureInfo{QueryStatus::Success, core14.dynamicRenderingLocalRead == VK_TRUE};
        report.features["hostImageCopy"] =
            FeatureInfo{QueryStatus::Success, core14.hostImageCopy == VK_TRUE};
    }

    std::expected<DescriptorHeapLimits, core::Error> queryHeapLimits(VkPhysicalDevice physical) {
        if (report.descriptorHeap != DescriptorHeapStatus::Supported ||
            !hasExtension(
                report.extensions,
                ExtensionScope::Device,
                VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME)) {
            return std::unexpected(makeError(
                core::ErrorCategory::QueryFailed,
                core::ErrorStage::Capability,
                "VK_EXT_descriptor_heap, its descriptorHeap feature, or its shader dependency is "
                "unavailable"));
        }
        if (vkGetPhysicalDeviceProperties2 == nullptr || vkGetDeviceProcAddr == nullptr) {
            return std::unexpected(makeError(
                core::ErrorCategory::LoaderSymbolMissing,
                core::ErrorStage::Capability,
                "Vulkan Descriptor Heap property or device lookup entry point is unavailable"));
        }

        VkPhysicalDeviceBufferDeviceAddressFeatures address{};
        address.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &address;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        if (address.bufferDeviceAddress != VK_TRUE) {
            return std::unexpected(makeError(
                core::ErrorCategory::QueryFailed,
                core::ErrorStage::Capability,
                "Descriptor Heap requires bufferDeviceAddress"));
        }

        VkPhysicalDeviceDescriptorHeapPropertiesEXT heap{};
        heap.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &heap;
        vkGetPhysicalDeviceProperties2(physical, &properties);
        if (heap.samplerHeapAlignment == 0 || heap.resourceHeapAlignment == 0 ||
            heap.maxSamplerHeapSize <= heap.minSamplerHeapReservedRange ||
            heap.maxResourceHeapSize <= heap.minResourceHeapReservedRange) {
            return std::unexpected(makeError(
                core::ErrorCategory::QueryFailed,
                core::ErrorStage::Capability,
                "Vulkan reports unusable Descriptor Heap limits"));
        }
        return DescriptorHeapLimits{
            heap.samplerHeapAlignment,
            heap.resourceHeapAlignment,
            heap.maxSamplerHeapSize,
            heap.maxResourceHeapSize,
            heap.minSamplerHeapReservedRange,
            heap.minResourceHeapReservedRange};
    }

    std::expected<void, core::Error>
    createDevice(VkPhysicalDevice physical, QueueFamilies families) {
        if (vkCreateDevice == nullptr || vkDestroyDevice == nullptr ||
            vkGetDeviceQueue == nullptr) {
            return std::unexpected(makeError(
                core::ErrorCategory::LoaderSymbolMissing,
                core::ErrorStage::Capability,
                "Vulkan logical device entry points are unavailable"));
        }
        constexpr float priority = 1.0F;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = families.graphics;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        std::array queues{queue, queue};
        queues[1].queueFamilyIndex = families.present;
        VkPhysicalDeviceVulkan13Features core13{};
        core13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        core13.dynamicRendering = VK_TRUE;
        core13.synchronization2 = VK_TRUE;
        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.pNext = &core13;
        info.queueCreateInfoCount = families.graphics == families.present ? 1u : 2u;
        info.pQueueCreateInfos = queues.data();

        std::vector<const char*> extensions;
        std::optional<DescriptorHeapLimits> heapLimits;
        VkPhysicalDeviceBufferDeviceAddressFeatures address{};
        VkPhysicalDeviceDescriptorHeapFeaturesEXT heap{};
        if (config.enableDescriptorHeap) {
            auto limits = queryHeapLimits(physical);
            if (!limits) {
                return std::unexpected(std::move(limits.error()));
            }
            heapLimits = *limits;
            address.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
            address.bufferDeviceAddress = VK_TRUE;
            heap.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
            heap.descriptorHeap = VK_TRUE;
            core13.pNext = &address;
            address.pNext = &heap;
            extensions.push_back(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
        }
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance{};
        if (surface != VK_NULL_HANDLE) {
            maintenance.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
            maintenance.swapchainMaintenance1 = VK_TRUE;
            maintenance.pNext = core13.pNext;
            core13.pNext = &maintenance;
            extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            extensions.push_back(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        }
        info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        const VkResult result = vkCreateDevice(physical, &info, nullptr, &graphicsDevice);
        if (result != VK_SUCCESS) {
            graphicsDevice = VK_NULL_HANDLE;
            return std::unexpected(makeError(
                core::ErrorCategory::InitializationFailed,
                core::ErrorStage::Capability,
                "vkCreateDevice failed",
                vulkanOrigin(result)));
        }
        vkGetDeviceQueue(graphicsDevice, families.graphics, 0, &graphicsQueue);
        if (graphicsQueue == VK_NULL_HANDLE) {
            return std::unexpected(makeError(
                core::ErrorCategory::InitializationFailed,
                core::ErrorStage::Capability,
                "vkGetDeviceQueue did not return the graphics queue"));
        }
        graphicsPhysical = physical;
        if (heapLimits) {
            for (const char* command :
                 {"vkWriteResourceDescriptorsEXT",
                  "vkWriteSamplerDescriptorsEXT",
                  "vkCmdBindResourceHeapEXT",
                  "vkCmdBindSamplerHeapEXT"}) {
                if (vkGetDeviceProcAddr(graphicsDevice, command) == nullptr) {
                    return std::unexpected(makeError(
                        core::ErrorCategory::LoaderSymbolMissing,
                        core::ErrorStage::Capability,
                        std::string("enabled Descriptor Heap has no device entry point: ") +
                            command));
                }
            }
            emitEvent(
                *diagnostics,
                core::LogLevel::Info,
                "rhi.device.descriptor-heap.enabled",
                "VK_EXT_descriptor_heap enabled and write/bind device entry points resolved");
        }
        graphicsInfo =
            GraphicsDeviceInfo{families.graphics, debugMessenger != VK_NULL_HANDLE, heapLimits};
        if (surface != VK_NULL_HANDLE) {
            VkQueue presentQueue = VK_NULL_HANDLE;
            vkGetDeviceQueue(graphicsDevice, families.present, 0, &presentQueue);
            if (!presentQueue) {
                return std::unexpected(makeError(
                    core::ErrorCategory::InitializationFailed,
                    core::ErrorStage::Capability,
                    "vkGetDeviceQueue did not return the present queue"));
            }
            presenter.emplace(graphicsDevice, graphicsQueue, presentQueue);
            if (auto opened = presenter->open(
                    physical,
                    surface,
                    families.graphics,
                    families.present,
                    {config.presentation->width, config.presentation->height},
                    config.triangleShaders ? &*config.triangleShaders : nullptr,
                    config.enableScreenshot);
                !opened) {
                return opened;
            }
            if (config.triangleShaders) {
                emitEvent(
                    *diagnostics,
                    core::LogLevel::Info,
                    "rhi.presentation.triangle-pipeline",
                    "Dynamic Rendering triangle pipeline created");
            }
            emitEvent(
                *diagnostics,
                core::LogLevel::Info,
                "rhi.presentation.ready",
                "Vulkan swapchain ready with " + std::to_string(presenter->imageCount()) +
                    " images and two frame contexts");
        }
        return {};
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
            report.errors.push_back(
                ReportError{
                    core::ErrorStage::Capability,
                    core::ErrorCategory::QueryFailed,
                    describe(result)});
            return;
        }
        std::vector<VkExtensionProperties> propertiesList(count);
        if (count > 0) {
            result = vkEnumerateDeviceExtensionProperties(
                device, nullptr, &count, propertiesList.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                report.deviceExtensionQuery.status = QueryStatus::Failed;
                report.deviceExtensionQuery.vkResult = describe(result);
                report.errors.push_back(
                    ReportError{
                        core::ErrorStage::Capability,
                        core::ErrorCategory::QueryFailed,
                        describe(result)});
                return;
            }
            propertiesList.resize(std::min<std::size_t>(propertiesList.size(), count));
        }
        report.deviceExtensionQuery.status = QueryStatus::Success;
        for (const VkExtensionProperties& property : propertiesList) {
            report.extensions.push_back(
                ExtensionInfo{
                    ExtensionScope::Device, property.extensionName, property.specVersion});
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
                report.variantSupported =
                    (*report.apiVariants.loader == 0 && *report.apiVariants.device == 0)
                        ? VariantSupport::True
                        : VariantSupport::False;
            }
        }

        const bool loaderEnough =
            report.loaderApi.has_value() && *report.loaderApi >= kApiVersion1_4;
        const bool deviceEnough =
            report.deviceApi.has_value() && *report.deviceApi >= kApiVersion1_4;
        const bool requestedEnough = config.requestedApi >= kApiVersion1_4;
        const bool created = report.instanceCreate.status == InstanceCreateStatus::Success;
        report.judgments.apiVersionSupported = loaderEnough && deviceEnough && requestedEnough &&
                                               created &&
                                               report.variantSupported == VariantSupport::True;

        const bool requiredPresent = std::all_of(
            config.requiredDeviceExtensions.begin(),
            config.requiredDeviceExtensions.end(),
            [&](const std::string& name) {
                return hasExtension(report.extensions, ExtensionScope::Device, name);
            });
        report.judgments.requiredCapabilitiesSupported = requiredPresent;

        if (report.loaderApi.has_value() && report.deviceApi.has_value()) {
            report.effectiveApi =
                std::min({*report.loaderApi, *report.deviceApi, config.requestedApi});
        }
    }

    // Releases whatever was acquired, in reverse order, and records what could not be released.
    void shutdown() {
        if (!std::exchange(ownsProcess, false)) {
            return;
        }
        bool retainPresentation = false;
        if (presenter) {
            if (auto error = presenter->close()) {
                noteCleanup(std::move(*error));
            }
            retainPresentation = presenter->hasSwapchain();
            presenter.reset();
        }
        // These buffers have no GPU consumers in the current slice.
        resources.reset();
        graphicsInfo.reset();
        if (graphicsDevice != VK_NULL_HANDLE && !retainPresentation) {
#ifdef AR_ENABLE_TEST_HOOKS
            g_process.shutdownOrder.push_back(4);
#endif
            vkDestroyDevice(graphicsDevice, nullptr);
            graphicsDevice = VK_NULL_HANDLE;
            graphicsQueue = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE && !retainPresentation) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (debugMessenger != VK_NULL_HANDLE) {
#ifdef AR_ENABLE_TEST_HOOKS
            g_process.shutdownOrder.push_back(5);
#endif
            vkDestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
            debugMessenger = VK_NULL_HANDLE;
        }
        if (retainPresentation) {
            retain(core::ResourceKind::Presentation);
            retain(core::ResourceKind::Instance);
            retain(core::ResourceKind::OsModule);
            g_process.quarantined = true;
            g_process.active = false;
            return;
        }
        if (instance != VK_NULL_HANDLE) {
#ifdef AR_ENABLE_TEST_HOOKS
            ++g_process.destroyInstanceCalls;
            g_process.shutdownOrder.push_back(1);
#endif
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
        if (!residualResources.empty()) {
            // The instance is still alive: unloading the module or clearing the volk table would
            // leave a live instance behind a dangling entry table. Keep both, quarantine the
            // process, and let process exit release them (proposal: instance and module retained).
            g_process.quarantined = true;
            g_process.active = false;
            return;
        }
        if (volkInitialized) {
#ifdef AR_ENABLE_TEST_HOOKS
            ++g_process.volkFinalizeCalls;
            g_process.shutdownOrder.push_back(2);
#endif
            volkFinalize();
            volkInitialized = false;
        }
        if (module != nullptr) {
#ifdef AR_ENABLE_TEST_HOOKS
            ++g_process.unloadCalls;
            g_process.shutdownOrder.push_back(3);
            bool unloaded = false;
            if (g_process.failUnloadOnce) {
                g_process.failUnloadOnce = false;
                ::SetLastError(ERROR_INVALID_HANDLE);
            } else {
                unloaded = ::FreeLibrary(module) != FALSE;
            }
#else
            const bool unloaded = ::FreeLibrary(module) != FALSE;
#endif
            if (!unloaded) {
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
        g_process.active = false;
    }

    std::expected<void, core::Error> query() {
        report.capabilitySource = sourceFor(config);
        report.apiVariants.requested = detail::apiVariant(config.requestedApi);
        report.instanceCreate.requested = config.requestedApi;

        // Checked before any OS load, volk initialisation or instance creation: a process that
        // retained resources must not reuse the previously loaded entry table.
#ifdef AR_ENABLE_TEST_HOOKS
        ++g_process.quarantineChecks;
#endif
        if (g_process.quarantined) {
            report.productTargetSupported = false;
            report.result = core::ResultKind::InitializationFailed;
            report.errors.push_back(
                ReportError{
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
        if (g_process.active) {
            return std::unexpected(makeError(
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Validation,
                "a Vulkan session is already active in this process"));
        }
        g_process.active = true;
        ownsProcess = true;

        emitEvent(
            *diagnostics,
            core::LogLevel::Info,
            "rhi.query.start",
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
        if (config.presentation) {
            if (auto created = createSurface(); !created) {
                return created;
            }
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
        std::optional<QueueFamilies> family;
        if (config.createGraphicsDevice) {
            auto selected = selectGraphicsDevice(*devices, *device);
            if (!selected) {
                return std::unexpected(std::move(selected.error()));
            }
            family = *selected;
        }
        queryDeviceFacts(*device);
        judge();

        if (report.deviceExtensionQuery.status == QueryStatus::Failed) {
            return std::unexpected(makeError(
                core::ErrorCategory::QueryFailed,
                core::ErrorStage::Capability,
                "the device extension list could not be queried"));
        }
        if (family) {
            if (config.requestedApi >= VK_API_VERSION_1_4) {
                queryVulkan14Features(*device);
            }
            if (auto created = createDevice(*device, *family); !created) {
                return created;
            }
        }

        report.productTargetSupported =
            report.judgments.apiVersionSupported && report.judgments.requiredCapabilitiesSupported;
        report.result = report.judgments.apiVersionSupported ? core::ResultKind::Success
                                                             : core::ResultKind::Unsupported;

        emitEvent(
            *diagnostics,
            core::LogLevel::Info,
            "rhi.query.complete",
            "device '" + report.deviceName + "' reports API " + apiText(*report.deviceApi) +
                ", descriptor heap " +
                (report.descriptorHeap == DescriptorHeapStatus::Supported ? "supported"
                                                                          : "not supported"),
            core::ErrorStage::Capability);
        return {};
    }

    QueryFailure failure(core::Error error) {
        shutdown();
        closed = true;
        report.productTargetSupported = false;
        report.result = resultKindFor(error.category);
        report.errors.push_back(
            ReportError{
                error.stage,
                error.category,
                error.origin.has_value() && error.origin->domain == "vulkan"
                    ? std::optional<VulkanResultInfo>(VulkanResultInfo{
                          static_cast<std::int32_t>(error.origin->code),
                          detail::vkResultName(static_cast<VkResult>(error.origin->code))})
                    : std::nullopt});
        emitEvent(
            *diagnostics,
            core::LogLevel::Error,
            "rhi.query.failed",
            error.message,
            error.stage,
            error.origin);
        QueryFailure result;
        result.error = std::move(error);
        result.cleanupErrors = cleanupErrors;
        // A rollback that could not release everything must report that fact upward; without it a
        // failed start would look like a clean release and the runtime summary would lose it.
        result.residualResources = residualResources;
        result.report = report;
        return result;
    }

    CloseResult doClose() {
        emitEvent(
            *diagnostics,
            core::LogLevel::Info,
            "rhi.close.start",
            "releasing Vulkan session resources",
            core::ErrorStage::Cleanup);
        shutdown();
        // Terminal for this session: a second close() returns the cached result, and a destructor
        // after an explicit close no longer retries the failed steps.
        closed = true;
        closeResult.closed = true;
        closeResult.errors = cleanupErrors;
        closeResult.residualResources = residualResources;
        closeResult.resourcesReleased = residualResources.empty();
        emitEvent(
            *diagnostics,
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

std::expected<std::unique_ptr<RhiSession>, QueryFailure>
RhiSession::open(const QueryConfig& config, core::DiagnosticSession& diagnostics) {
    auto impl = std::make_unique<Impl>(diagnostics);
    impl->config = config;
    auto session = std::unique_ptr<RhiSession>(new RhiSession(std::move(impl)));
    if (auto queried = session->m_impl->query(); !queried) {
        return std::unexpected(session->m_impl->failure(std::move(queried.error())));
    }
    return session;
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

std::optional<GraphicsDeviceInfo> RhiSession::graphicsDeviceInfo() const noexcept {
    return m_impl->graphicsInfo;
}

std::expected<PresentStatus, core::Error>
RhiSession::presentFrame(std::uint32_t width, std::uint32_t height, FrameScreenshot* screenshot) {
    if (!m_impl->presenter) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidState,
            core::ErrorStage::Capability,
            "this RHI session has no active swapchain"));
    }
    return m_impl->presenter->presentFrame({width, height}, screenshot);
}

std::expected<BufferHandle, core::Error> RhiSession::createBuffer(BufferDesc desc) {
    if (m_impl->closed || !m_impl->graphicsDevice || m_impl->report.capabilitySource.simulated) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidState,
            core::ErrorStage::Capability,
            "buffer creation requires an open real graphics device"));
    }
    if (!m_impl->resources) {
        m_impl->resources.emplace(
            m_impl->instance,
            m_impl->graphicsPhysical,
            m_impl->graphicsDevice,
            compatibilityVersion(m_impl->config.requestedApi));
    }
    return m_impl->resources->createBuffer(desc);
}

std::expected<void, core::Error> RhiSession::writeBuffer(
    BufferHandle handle, std::uint64_t offset, std::span<const std::byte> bytes) {
    if (!m_impl->resources) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidState,
            core::ErrorStage::Capability,
            "there are no active buffer resources"));
    }
    return m_impl->resources->writeBuffer(handle, offset, bytes);
}

std::expected<void, core::Error>
RhiSession::readBuffer(BufferHandle handle, std::uint64_t offset, std::span<std::byte> bytes) {
    if (!m_impl->resources) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidState,
            core::ErrorStage::Capability,
            "there are no active buffer resources"));
    }
    return m_impl->resources->readBuffer(handle, offset, bytes);
}

std::expected<void, core::Error> RhiSession::destroyBuffer(BufferHandle handle) {
    if (!m_impl->resources) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidState,
            core::ErrorStage::Capability,
            "there are no active buffer resources"));
    }
    return m_impl->resources->destroyBuffer(handle);
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
    return ProcessCounters{
        g_process.loadCalls,
        g_process.volkInitializeCalls,
        g_process.instanceCreateCalls,
        g_process.destroyInstanceCalls,
        g_process.volkFinalizeCalls,
        g_process.unloadCalls,
        g_process.reportCallsAfterClose,
        g_process.quarantineChecks};
}

void resetProcessCounters() noexcept {
    g_process.loadCalls = 0;
    g_process.volkInitializeCalls = 0;
    g_process.instanceCreateCalls = 0;
    g_process.destroyInstanceCalls = 0;
    g_process.volkFinalizeCalls = 0;
    g_process.unloadCalls = 0;
    g_process.reportCallsAfterClose = 0;
    g_process.quarantineChecks = 0;
    g_process.shutdownOrder.clear();
}

std::vector<int> processShutdownOrder() {
    return g_process.shutdownOrder;
}

void failNextUnloadForTesting() noexcept {
    g_process.failUnloadOnce = true;
}

} // namespace detail

#endif // AR_ENABLE_TEST_HOOKS

} // namespace ar::rhi
