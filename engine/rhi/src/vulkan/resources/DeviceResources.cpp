#include "DeviceResources.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include "VulkanResult.h"

namespace ar::rhi::detail {
namespace {

// RHI sessions exclusively use the process loader on their owner thread.
// Process-wide generations also reject a handle from a closed session.
std::uint64_t nextGeneration = 1;

core::Error invalid(const char* message) {
    return {
        core::ErrorCategory::InvalidArguments, core::ErrorStage::Capability, message, std::nullopt};
}

core::Error failed(const char* operation, VkResult result) {
    return {
        core::ErrorCategory::InitializationFailed,
        core::ErrorStage::Capability,
        std::string(operation) + " returned " + vkResultName(result),
        core::ErrorOrigin{"vulkan", result}};
}

template <typename Slots> std::expected<std::uint32_t, core::Error> reserveSlot(Slots& slots) {
    if (nextGeneration == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(invalid("resource handle generations exhausted"));
    }
    const auto free =
        std::ranges::find_if(slots, [](const auto& slot) { return slot.generation == 0; });
    const auto index = static_cast<std::size_t>(free - slots.begin());
    if (index == slots.size()) {
        if (index == std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(invalid("resource slots exhausted"));
        }
        try {
            slots.emplace_back();
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                core::Error{
                    core::ErrorCategory::InitializationFailed,
                    core::ErrorStage::Capability,
                    "not enough host memory to reserve a resource slot",
                    std::nullopt});
        }
    }
    return static_cast<std::uint32_t>(index);
}

template <typename Slots, typename Handle>
typename Slots::value_type* findSlot(Slots& slots, Handle handle) noexcept {
    if (!handle.generation || handle.slot >= slots.size()) {
        return nullptr;
    }
    auto& slot = slots[handle.slot];
    return slot.generation == handle.generation ? &slot : nullptr;
}

constexpr VkFormat kImageFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags kImageUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

} // namespace

DeviceResources::DeviceResources(
    VkInstance instance,
    VkPhysicalDevice physical,
    VkDevice device,
    std::uint32_t apiVersion) noexcept
    : m_instance(instance), m_physical(physical), m_device(device), m_apiVersion(apiVersion) {}

DeviceResources::~DeviceResources() {
    if (!m_allocator) {
        return;
    }
    for (const auto& slot : m_views) {
        if (slot.view) {
            vkDestroyImageView(m_device, slot.view, nullptr);
        }
    }
    for (const auto& slot : m_images) {
        if (slot.image) {
            vmaDestroyImage(m_allocator, slot.image, slot.allocation);
        }
    }
    for (const auto& slot : m_buffers) {
        if (slot.buffer) {
            vmaDestroyBuffer(m_allocator, slot.buffer, slot.allocation);
        }
    }
    vmaDestroyAllocator(m_allocator);
}

std::expected<void, core::Error> DeviceResources::ensureAllocator() {
    if (m_allocator) {
        return {};
    }
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo info{};
    info.instance = m_instance;
    info.physicalDevice = m_physical;
    info.device = m_device;
    info.vulkanApiVersion = m_apiVersion;
    info.pVulkanFunctions = &functions;
    VmaAllocator allocator = VK_NULL_HANDLE;
    const VkResult result = vmaCreateAllocator(&info, &allocator);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateAllocator", result));
    }
    m_allocator = allocator;
    return {};
}

std::expected<BufferHandle, core::Error> DeviceResources::createBuffer(BufferDesc desc) {
    if (!desc.bytes ||
        (desc.memory != BufferMemory::HostVisible && desc.memory != BufferMemory::DeviceLocal)) {
        return std::unexpected(invalid("buffer requires a positive size and a valid memory class"));
    }
    if (!m_maxBufferSize) {
        VkPhysicalDeviceMaintenance4Properties limits{};
        limits.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &limits;
        vkGetPhysicalDeviceProperties2(m_physical, &properties);
        m_maxBufferSize = limits.maxBufferSize;
    }
    if (desc.bytes > m_maxBufferSize) {
        return std::unexpected(invalid("buffer size exceeds the device limit"));
    }
    if (auto initialized = ensureAllocator(); !initialized) {
        return std::unexpected(std::move(initialized.error()));
    }
    const auto index = reserveSlot(m_buffers);
    if (!index) {
        return std::unexpected(index.error());
    }

    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = desc.bytes;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    if (desc.memory == BufferMemory::HostVisible) {
        allocation.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    } else {
        allocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
    BufferSlot created;
    const VkResult result = vmaCreateBuffer(
        m_allocator, &buffer, &allocation, &created.buffer, &created.allocation, nullptr);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateBuffer", result));
    }
    created.desc = desc;
    created.generation = nextGeneration++;
    m_buffers[*index] = created;
    return BufferHandle{*index, created.generation};
}

std::expected<DeviceResources::BufferSlot*, core::Error>
DeviceResources::validateAccess(BufferHandle handle, std::uint64_t offset, std::size_t bytes) {
    auto* slot = findSlot(m_buffers, handle);
    if (!slot) {
        return std::unexpected(invalid("buffer handle is invalid or expired"));
    }
    if (slot->desc.memory != BufferMemory::HostVisible) {
        return std::unexpected(invalid("direct CPU access requires a host-visible buffer"));
    }
    if (offset > slot->desc.bytes || bytes > slot->desc.bytes - offset) {
        return std::unexpected(invalid("buffer access is outside the resource range"));
    }
    return slot;
}

std::expected<void, core::Error> DeviceResources::writeBuffer(
    BufferHandle handle, std::uint64_t offset, std::span<const std::byte> bytes) {
    auto slot = validateAccess(handle, offset, bytes.size());
    if (!slot) {
        return std::unexpected(std::move(slot.error()));
    }
    if (bytes.empty()) {
        return {};
    }
    const auto allocation = (*slot)->allocation;
    void* mapped = nullptr;
    VkResult result = vmaMapMemory(m_allocator, allocation, &mapped);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaMapMemory", result));
    }
    std::memcpy(static_cast<std::byte*>(mapped) + offset, bytes.data(), bytes.size());
    result = vmaFlushAllocation(m_allocator, allocation, offset, bytes.size());
    vmaUnmapMemory(m_allocator, allocation);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaFlushAllocation", result));
    }
    return {};
}

std::expected<void, core::Error>
DeviceResources::readBuffer(BufferHandle handle, std::uint64_t offset, std::span<std::byte> bytes) {
    auto slot = validateAccess(handle, offset, bytes.size());
    if (!slot) {
        return std::unexpected(std::move(slot.error()));
    }
    if (bytes.empty()) {
        return {};
    }
    const auto allocation = (*slot)->allocation;
    void* mapped = nullptr;
    VkResult result = vmaMapMemory(m_allocator, allocation, &mapped);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaMapMemory", result));
    }
    result = vmaInvalidateAllocation(m_allocator, allocation, offset, bytes.size());
    if (result == VK_SUCCESS) {
        std::memcpy(bytes.data(), static_cast<const std::byte*>(mapped) + offset, bytes.size());
    }
    vmaUnmapMemory(m_allocator, allocation);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaInvalidateAllocation", result));
    }
    return {};
}

std::expected<void, core::Error> DeviceResources::destroyBuffer(BufferHandle handle) {
    auto* slot = findSlot(m_buffers, handle);
    if (!slot) {
        return std::unexpected(invalid("buffer handle is invalid or expired"));
    }
    vmaDestroyBuffer(m_allocator, slot->buffer, slot->allocation);
    *slot = {};
    return {};
}

std::expected<ImageHandle, core::Error> DeviceResources::createImage(ImageDesc desc) {
    if (!desc.width || !desc.height) {
        return std::unexpected(invalid("image dimensions must be positive"));
    }
    VkImageFormatProperties limits{};
    const VkResult supported = vkGetPhysicalDeviceImageFormatProperties(
        m_physical,
        kImageFormat,
        VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        kImageUsage,
        0,
        &limits);
    if (supported != VK_SUCCESS) {
        return std::unexpected(failed("vkGetPhysicalDeviceImageFormatProperties", supported));
    }
    if (desc.width > limits.maxExtent.width || desc.height > limits.maxExtent.height) {
        return std::unexpected(invalid("image dimensions exceed the device limit"));
    }
    if (auto initialized = ensureAllocator(); !initialized) {
        return std::unexpected(std::move(initialized.error()));
    }
    const auto index = reserveSlot(m_images);
    if (!index) {
        return std::unexpected(index.error());
    }
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = kImageFormat;
    info.extent = {desc.width, desc.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = kImageUsage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    ImageSlot created;
    const VkResult result = vmaCreateImage(
        m_allocator, &info, &allocation, &created.image, &created.allocation, nullptr);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateImage", result));
    }
    created.generation = nextGeneration++;
    m_images[*index] = created;
    return ImageHandle{*index, created.generation};
}

std::expected<ImageViewHandle, core::Error> DeviceResources::createImageView(ImageHandle image) {
    const auto* source = findSlot(m_images, image);
    if (!source) {
        return std::unexpected(invalid("image handle is invalid or expired"));
    }
    const auto index = reserveSlot(m_views);
    if (!index) {
        return std::unexpected(index.error());
    }
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = source->image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = kImageFormat;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ViewSlot created;
    const VkResult result = vkCreateImageView(m_device, &info, nullptr, &created.view);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateImageView", result));
    }
    created.image = image;
    created.generation = nextGeneration++;
    m_views[*index] = created;
    return ImageViewHandle{*index, created.generation};
}

std::expected<void, core::Error> DeviceResources::destroyImageView(ImageViewHandle handle) {
    auto* slot = findSlot(m_views, handle);
    if (!slot) {
        return std::unexpected(invalid("image view handle is invalid or expired"));
    }
    vkDestroyImageView(m_device, slot->view, nullptr);
    *slot = {};
    return {};
}

std::expected<void, core::Error> DeviceResources::destroyImage(ImageHandle handle) {
    auto* slot = findSlot(m_images, handle);
    if (!slot) {
        return std::unexpected(invalid("image handle is invalid or expired"));
    }
    if (std::ranges::any_of(
            m_views, [handle](const auto& view) { return view.view && view.image == handle; })) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "destroy the image views before destroying their image",
                std::nullopt});
    }
    vmaDestroyImage(m_allocator, slot->image, slot->allocation);
    *slot = {};
    return {};
}

} // namespace ar::rhi::detail
