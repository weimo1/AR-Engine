#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

#include <volk.h>

#include "core/Error.h"
#include "rhi/Buffer.h"

struct VmaAllocation_T;
struct VmaAllocator_T;

namespace ar::rhi::detail {

// Borrows a live Vulkan device and its parents. Destroy on the session owner thread
// before destroying the device.
class DeviceResources {
public:
    DeviceResources(
        VkInstance instance,
        VkPhysicalDevice physical,
        VkDevice device,
        std::uint32_t apiVersion) noexcept;
    ~DeviceResources();

    DeviceResources(const DeviceResources&) = delete;
    DeviceResources& operator=(const DeviceResources&) = delete;

    std::expected<BufferHandle, core::Error> createBuffer(BufferDesc desc);
    std::expected<void, core::Error>
    writeBuffer(BufferHandle handle, std::uint64_t offset, std::span<const std::byte> bytes);
    std::expected<void, core::Error>
    readBuffer(BufferHandle handle, std::uint64_t offset, std::span<std::byte> bytes);
    std::expected<void, core::Error> destroyBuffer(BufferHandle handle);

private:
    struct BufferSlot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        BufferDesc desc{};
        std::uint64_t generation = 0;
    };

    std::expected<void, core::Error> initializeAllocator();
    BufferSlot* find(BufferHandle handle) noexcept;
    std::expected<BufferSlot*, core::Error>
    validateAccess(BufferHandle handle, std::uint64_t offset, std::size_t bytes);

    VkInstance m_instance;
    VkPhysicalDevice m_physical;
    VkDevice m_device;
    std::uint32_t m_apiVersion;
    VkDeviceSize m_maxBufferSize = 0;
    VmaAllocator_T* m_allocator = nullptr;
    std::vector<BufferSlot> m_buffers;
};

} // namespace ar::rhi::detail
