// A fake Vulkan loader used only by the rhi tests.
//
// The rhi session loads whatever module the caller names in ExplicitLoaderPath and takes
// every entry point from that module's vkGetInstanceProcAddr, so a stub with the same export
// is enough to exercise loader, instance, enumeration and cleanup paths without a GPU.
//
// Behaviour is selected per process through AR_RHI_FIXTURE_SPEC, for example
//   devices=0;create=ok;instance-ext=ok
// Keys (all optional):
//   loader=MAJOR.MINOR      loader API version (default 1.4)
//   loader-variant=N        loader API variant (default 0)
//   device=MAJOR.MINOR      device API version (default 1.4)
//   create=ok|incompatible|init-failed
//   devices=N               number of enumerated physical devices (default 1)
//   enumerate=ok|incomplete second call keeps returning VK_INCOMPLETE
//   instance-ext=ok|failed
//   device-ext=ok|failed
//   destroy=ok|missing      omit vkDestroyInstance from vkGetInstanceProcAddr
//   heap=present|absent     advertise VK_EXT_descriptor_heap and its feature

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The fixture implements the entry points itself, so the headers must not declare them.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

struct Spec {
    std::uint32_t loaderApi = VK_MAKE_API_VERSION(0, 1, 4, 0);
    std::uint32_t deviceApi = VK_MAKE_API_VERSION(0, 1, 4, 0);
    VkResult createResult = VK_SUCCESS;
    std::uint32_t deviceCount = 1;
    bool enumerateIncomplete = false;
    bool instanceExtensionsFail = false;
    bool deviceExtensionsFail = false;
    bool destroyUnavailable = false;
    bool descriptorHeapPresent = false;
    std::string dynamicRendering;
    // Raw device-name bytes as hex, so a test can hand the runtime text that is not valid UTF-8.
    std::string deviceNameHex;
};

std::uint32_t parseVersion(std::string_view text) {
    if (text.empty()) {
        return VK_MAKE_API_VERSION(0, 1, 4, 0);
    }
    const std::size_t dot = text.find('.');
    const std::string_view majorText = text.substr(0, dot);
    const std::string_view minorText =
        dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    const auto major =
        static_cast<std::uint32_t>(std::strtoul(std::string(majorText).c_str(), nullptr, 10));
    const auto minor =
        minorText.empty()
            ? std::uint32_t{0}
            : static_cast<std::uint32_t>(std::strtoul(std::string(minorText).c_str(), nullptr, 10));
    return VK_MAKE_API_VERSION(0, major, minor, 0);
}

Spec readSpec() {
    Spec spec;
    char* buffer = nullptr;
    std::size_t size = 0;
    if (::_dupenv_s(&buffer, &size, "AR_RHI_FIXTURE_SPEC") != 0 || buffer == nullptr) {
        return spec;
    }
    std::string text(buffer);
    std::free(buffer);
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(';', start);
        const std::string entry = text.substr(start, end == std::string::npos ? end : end - start);
        start = end == std::string::npos ? text.size() + 1 : end + 1;
        if (entry.empty()) {
            continue;
        }
        const std::size_t equals = entry.find('=');
        const std::string key = entry.substr(0, equals);
        const std::string value =
            equals == std::string::npos ? std::string{} : entry.substr(equals + 1);
        if (key == "loader") {
            spec.loaderApi = parseVersion(value);
        } else if (key == "loader-variant") {
            const auto variant =
                static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
            spec.loaderApi = VK_MAKE_API_VERSION(variant, 1, 4, 0);
        } else if (key == "device") {
            spec.deviceApi = parseVersion(value);
        } else if (key == "create") {
            if (value == "incompatible") {
                spec.createResult = VK_ERROR_INCOMPATIBLE_DRIVER;
            } else if (value == "init-failed") {
                spec.createResult = VK_ERROR_INITIALIZATION_FAILED;
            }
        } else if (key == "devices") {
            spec.deviceCount = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "enumerate") {
            spec.enumerateIncomplete = value == "incomplete";
        } else if (key == "instance-ext") {
            spec.instanceExtensionsFail = value == "failed";
        } else if (key == "device-ext") {
            spec.deviceExtensionsFail = value == "failed";
        } else if (key == "destroy") {
            spec.destroyUnavailable = value == "missing";
        } else if (key == "heap") {
            spec.descriptorHeapPresent = value == "present";
        } else if (key == "dynamic") {
            spec.dynamicRendering = value;
        } else if (key == "device-name-hex") {
            spec.deviceNameHex = value;
        }
    }
    return spec;
}

// Read on every call: the tests reuse one process (and one loaded module) across sections, so a
// cached snapshot would leak the first section's behaviour into the ones after it.
Spec currentSpec() {
    return readSpec();
}

VKAPI_ATTR VkResult VKAPI_CALL fixtureEnumerateInstanceVersion(std::uint32_t* apiVersion) {
    if (apiVersion != nullptr) {
        *apiVersion = currentSpec().loaderApi;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fixtureCreateInstance(
    const VkInstanceCreateInfo* info, const VkAllocationCallbacks*, VkInstance* instance) {
    const Spec spec = currentSpec();
    if (info->enabledLayerCount != 0 &&
        std::string_view(info->ppEnabledLayerNames[0]) != "VK_LAYER_KHRONOS_validation") {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    if (info->enabledExtensionCount != 0 &&
        std::string_view(info->ppEnabledExtensionNames[0]) != VK_EXT_DEBUG_UTILS_EXTENSION_NAME) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (instance != nullptr) {
        *instance = spec.createResult == VK_SUCCESS
                        ? reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(1))
                        : VK_NULL_HANDLE;
    }
    return spec.createResult;
}

VKAPI_ATTR VkResult VKAPI_CALL fixtureEnumerateInstanceExtensionProperties(
    const char*, std::uint32_t* count, VkExtensionProperties* properties) {
    const Spec spec = currentSpec();
    if (spec.instanceExtensionsFail) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (properties == nullptr) {
        *count = 0;
        return VK_SUCCESS;
    }
    *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fixtureDestroyInstance(VkInstance, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL
fixtureEnumeratePhysicalDevices(VkInstance, std::uint32_t* count, VkPhysicalDevice* devices) {
    const Spec spec = currentSpec();
    if (devices == nullptr) {
        *count = spec.deviceCount;
        return VK_SUCCESS;
    }
    if (spec.enumerateIncomplete) {
        return VK_INCOMPLETE;
    }
    const std::uint32_t offered = *count;
    std::uint32_t written = 0;
    for (std::uint32_t index = 0; index < spec.deviceCount && index < offered; ++index) {
        devices[index] =
            reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(0x10 + index));
        ++written;
    }
    *count = written;
    return written < offered ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
fixtureGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* properties) {
    if (properties == nullptr) {
        return;
    }
    const Spec spec = currentSpec();
    properties->apiVersion = spec.deviceApi;
    properties->driverVersion = 1;
    std::memset(properties->deviceName, 0, sizeof(properties->deviceName));
    if (spec.deviceNameHex.empty()) {
        const char* name = "AR Engine fixture device";
        std::memcpy(properties->deviceName, name, std::strlen(name));
        return;
    }
    std::string bytes;
    for (std::size_t index = 0; index + 1 < spec.deviceNameHex.size(); index += 2) {
        const std::string pair = spec.deviceNameHex.substr(index, 2);
        bytes.push_back(static_cast<char>(std::strtoul(pair.c_str(), nullptr, 16)));
    }
    const std::size_t limit = sizeof(properties->deviceName) - 1;
    std::memcpy(properties->deviceName, bytes.data(), std::min(bytes.size(), limit));
}

VKAPI_ATTR VkResult VKAPI_CALL fixtureEnumerateDeviceExtensionProperties(
    VkPhysicalDevice, const char*, std::uint32_t* count, VkExtensionProperties* properties) {
    const Spec spec = currentSpec();
    if (spec.deviceExtensionsFail) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const std::uint32_t available = spec.descriptorHeapPresent ? 1U : 0U;
    if (properties == nullptr) {
        *count = available;
        return VK_SUCCESS;
    }
    std::uint32_t written = 0;
    if (available > 0 && *count > 0) {
        std::memset(properties[0].extensionName, 0, sizeof(properties[0].extensionName));
        const char* name = "VK_EXT_descriptor_heap";
        std::memcpy(properties[0].extensionName, name, std::strlen(name));
        properties[0].specVersion = 1;
        written = 1;
    }
    *count = written;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
fixtureGetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2* features) {
    if (features == nullptr) {
        return;
    }
    const Spec spec = currentSpec();
    auto* next = static_cast<VkBaseOutStructure*>(features->pNext);
    while (next != nullptr) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            auto* core13 = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(next);
            const bool first = reinterpret_cast<std::uintptr_t>(physical) == 0x10;
            const bool missing = spec.dynamicRendering == "missing" ||
                                 (first && spec.dynamicRendering == "first-missing");
            core13->dynamicRendering = missing ? VK_FALSE : VK_TRUE;
            core13->synchronization2 = VK_TRUE;
        }
#if defined(VK_EXT_descriptor_heap)
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT) {
            auto* heap = reinterpret_cast<VkPhysicalDeviceDescriptorHeapFeaturesEXT*>(next);
            heap->descriptorHeap = spec.descriptorHeapPresent ? VK_TRUE : VK_FALSE;
        }
#endif
        next = next->pNext;
    }
}

VkDebugUtilsMessengerCreateInfoEXT fixtureMessenger{};

VKAPI_ATTR VkResult VKAPI_CALL fixtureCreateMessenger(
    VkInstance,
    const VkDebugUtilsMessengerCreateInfoEXT* info,
    const VkAllocationCallbacks*,
    VkDebugUtilsMessengerEXT* messenger) {
    fixtureMessenger = *info;
    *messenger = reinterpret_cast<VkDebugUtilsMessengerEXT>(static_cast<std::uintptr_t>(0x20));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
fixtureDestroyMessenger(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks*) {
    fixtureMessenger = {};
}

VKAPI_ATTR void VKAPI_CALL fixtureGetQueueFamilies(
    VkPhysicalDevice, std::uint32_t* count, VkQueueFamilyProperties* properties) {
    if (properties == nullptr) {
        *count = 2;
        return;
    }
    *count = std::min(*count, 2U);
    if (*count > 0) {
        properties[0].queueCount = 1;
        properties[0].queueFlags = VK_QUEUE_COMPUTE_BIT;
    }
    if (*count > 1) {
        properties[1].queueCount = 1;
        properties[1].queueFlags = VK_QUEUE_GRAPHICS_BIT;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL fixtureCreateDevice(
    VkPhysicalDevice physical,
    const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks*,
    VkDevice* device) {
    const Spec spec = currentSpec();
    const bool first = reinterpret_cast<std::uintptr_t>(physical) == 0x10;
    if (spec.dynamicRendering == "missing" || (first && spec.dynamicRendering == "first-missing")) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const auto* core13 = static_cast<const VkPhysicalDeviceVulkan13Features*>(info->pNext);
    if (core13 == nullptr ||
        core13->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES ||
        core13->dynamicRendering != VK_TRUE || core13->synchronization2 != VK_TRUE ||
        info->queueCreateInfoCount != 1 || info->pQueueCreateInfos[0].queueFamilyIndex != 1 ||
        info->pQueueCreateInfos[0].queueCount != 1) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (fixtureMessenger.pfnUserCallback != nullptr) {
        VkDebugUtilsMessengerCallbackDataEXT message{};
        message.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
        message.pMessage = "fixture warning during vkCreateDevice";
        fixtureMessenger.pfnUserCallback(
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            &message,
            fixtureMessenger.pUserData);
    }
    *device = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0x30));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
fixtureGetDeviceQueue(VkDevice, std::uint32_t family, std::uint32_t index, VkQueue* queue) {
    *queue = family == 1 && index == 0
                 ? reinterpret_cast<VkQueue>(static_cast<std::uintptr_t>(0x40))
                 : VK_NULL_HANDLE;
}

VKAPI_ATTR void VKAPI_CALL fixtureDestroyDevice(VkDevice, const VkAllocationCallbacks*) {}

} // namespace

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance, const char* name) {
    const Spec spec = currentSpec();
    if (name == nullptr) {
        return nullptr;
    }
    const std::string_view requested(name);
    if (requested == "vkGetInstanceProcAddr") {
        return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
    }
    if (requested == "vkEnumerateInstanceVersion") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureEnumerateInstanceVersion);
    }
    if (requested == "vkCreateInstance") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureCreateInstance);
    }
    if (requested == "vkEnumerateInstanceExtensionProperties") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureEnumerateInstanceExtensionProperties);
    }
    if (requested == "vkEnumeratePhysicalDevices") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureEnumeratePhysicalDevices);
    }
    if (requested == "vkGetPhysicalDeviceProperties") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureGetPhysicalDeviceProperties);
    }
    if (requested == "vkEnumerateDeviceExtensionProperties") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureEnumerateDeviceExtensionProperties);
    }
    if (requested == "vkGetPhysicalDeviceFeatures2") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureGetPhysicalDeviceFeatures2);
    }
    if (requested == "vkCreateDebugUtilsMessengerEXT") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureCreateMessenger);
    }
    if (requested == "vkDestroyDebugUtilsMessengerEXT") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureDestroyMessenger);
    }
    if (requested == "vkGetPhysicalDeviceQueueFamilyProperties") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureGetQueueFamilies);
    }
    if (requested == "vkCreateDevice") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureCreateDevice);
    }
    if (requested == "vkGetDeviceQueue") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureGetDeviceQueue);
    }
    if (requested == "vkDestroyDevice") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureDestroyDevice);
    }
    if (requested == "vkDestroyInstance") {
        if (spec.destroyUnavailable) {
            return nullptr;
        }
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureDestroyInstance);
    }
    return nullptr;
}
