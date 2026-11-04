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
};

std::uint32_t parseVersion(std::string_view text) {
    if (text.empty()) {
        return VK_MAKE_API_VERSION(0, 1, 4, 0);
    }
    const std::size_t dot = text.find('.');
    const std::string_view majorText = text.substr(0, dot);
    const std::string_view minorText =
        dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    const auto major = static_cast<std::uint32_t>(std::strtoul(std::string(majorText).c_str(), nullptr, 10));
    const auto minor = minorText.empty()
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
        const std::string value = equals == std::string::npos ? std::string{} : entry.substr(equals + 1);
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
        }
    }
    return spec;
}

const Spec& currentSpec() {
    static const Spec spec = readSpec();
    return spec;
}

VKAPI_ATTR VkResult VKAPI_CALL fixtureEnumerateInstanceVersion(std::uint32_t* apiVersion) {
    if (apiVersion != nullptr) {
        *apiVersion = currentSpec().loaderApi;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fixtureCreateInstance(
    const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance* instance) {
    const Spec& spec = currentSpec();
    if (instance != nullptr) {
        *instance = spec.createResult == VK_SUCCESS
            ? reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(1))
            : VK_NULL_HANDLE;
    }
    return spec.createResult;
}

VKAPI_ATTR VkResult VKAPI_CALL fixtureEnumerateInstanceExtensionProperties(
    const char*, std::uint32_t* count, VkExtensionProperties* properties) {
    const Spec& spec = currentSpec();
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

VKAPI_ATTR VkResult VKAPI_CALL fixtureEnumeratePhysicalDevices(
    VkInstance, std::uint32_t* count, VkPhysicalDevice* devices) {
    const Spec& spec = currentSpec();
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
        devices[index] = reinterpret_cast<VkPhysicalDevice>(
            static_cast<std::uintptr_t>(0x10 + index));
        ++written;
    }
    *count = written;
    return written < offered ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fixtureGetPhysicalDeviceProperties(
    VkPhysicalDevice, VkPhysicalDeviceProperties* properties) {
    if (properties == nullptr) {
        return;
    }
    properties->apiVersion = currentSpec().deviceApi;
    properties->driverVersion = 1;
    std::memset(properties->deviceName, 0, sizeof(properties->deviceName));
    const char* name = "AR Engine fixture device";
    std::memcpy(properties->deviceName, name, std::strlen(name));
}

VKAPI_ATTR VkResult VKAPI_CALL fixtureEnumerateDeviceExtensionProperties(
    VkPhysicalDevice, const char*, std::uint32_t* count, VkExtensionProperties* properties) {
    const Spec& spec = currentSpec();
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

#if defined(VK_EXT_descriptor_heap)
VKAPI_ATTR void VKAPI_CALL fixtureGetPhysicalDeviceFeatures2(
    VkPhysicalDevice, VkPhysicalDeviceFeatures2* features) {
    if (features == nullptr) {
        return;
    }
    auto* heapHead = reinterpret_cast<VkBaseOutStructure*>(features->pNext);
    while (heapHead != nullptr) {
        if (heapHead->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT) {
            auto* heap = reinterpret_cast<VkPhysicalDeviceDescriptorHeapFeaturesEXT*>(heapHead);
            heap->descriptorHeap = currentSpec().descriptorHeapPresent ? VK_TRUE : VK_FALSE;
            break;
        }
        heapHead = heapHead->pNext;
    }
}
#endif

} // namespace

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance, const char* name) {
    const Spec& spec = currentSpec();
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
#if defined(VK_EXT_descriptor_heap)
    if (requested == "vkGetPhysicalDeviceFeatures2") {
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureGetPhysicalDeviceFeatures2);
    }
#endif
    if (requested == "vkDestroyInstance") {
        if (spec.destroyUnavailable) {
            return nullptr;
        }
        return reinterpret_cast<PFN_vkVoidFunction>(&fixtureDestroyInstance);
    }
    return nullptr;
}
