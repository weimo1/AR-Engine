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
    for (const auto& slot : m_buffers) {
        if (slot.buffer) {
            vmaDestroyBuffer(m_allocator, slot.buffer, slot.allocation);
        }
    }
    vmaDestroyAllocator(m_allocator);
}

std::expected<void, core::Error> DeviceResources::initializeAllocator() {
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
    if (!m_allocator) {
        if (auto initialized = initializeAllocator(); !initialized) {
            return std::unexpected(std::move(initialized.error()));
        }
    }
    if (nextGeneration == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(invalid("buffer handle generations exhausted"));
    }
    const auto free = std::ranges::find_if(
        m_buffers, [](const auto& slot) { return slot.buffer == VK_NULL_HANDLE; });
    const auto index = static_cast<std::size_t>(free - m_buffers.begin());
    if (index == m_buffers.size()) {
        if (index == std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(invalid("buffer slots exhausted"));
        }
        try {
            m_buffers.emplace_back();
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                core::Error{
                    core::ErrorCategory::InitializationFailed,
                    core::ErrorStage::Capability,
                    "not enough host memory to reserve a buffer slot",
                    std::nullopt});
        }
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
    m_buffers[index] = created;
    return BufferHandle{static_cast<std::uint32_t>(index), created.generation};
}

DeviceResources::BufferSlot* DeviceResources::find(BufferHandle handle) noexcept {
    if (handle.slot >= m_buffers.size()) {
        return nullptr;
    }
    auto& slot = m_buffers[handle.slot];
    return slot.buffer && slot.generation == handle.generation ? &slot : nullptr;
}

std::expected<DeviceResources::BufferSlot*, core::Error>
DeviceResources::validateAccess(BufferHandle handle, std::uint64_t offset, std::size_t bytes) {
    auto* slot = find(handle);
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
    auto* slot = find(handle);
    if (!slot) {
        return std::unexpected(invalid("buffer handle is invalid or expired"));
    }
    vmaDestroyBuffer(m_allocator, slot->buffer, slot->allocation);
    *slot = {};
    return {};
}

} // namespace ar::rhi::detail
