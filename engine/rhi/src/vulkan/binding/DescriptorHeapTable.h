#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

#include <volk.h>

#include "binding/DescriptorHeapLayout.h"
#include "core/Error.h"

struct VmaAllocation_T;

namespace ar::rhi::detail {

class DeviceResources;

struct DescriptorHeapDispatch {
    PFN_vkWriteResourceDescriptorsEXT writeResourceDescriptors = nullptr;
    PFN_vkWriteSamplerDescriptorsEXT writeSamplerDescriptors = nullptr;
    PFN_vkCmdBindResourceHeapEXT cmdBindResourceHeap = nullptr;
    PFN_vkCmdBindSamplerHeapEXT cmdBindSamplerHeap = nullptr;
    PFN_vkCmdPushDataEXT cmdPushData = nullptr;
};

// Owns aligned VMA-backed Resource Heap and Sampler Heap buffers for VK_EXT_descriptor_heap.
// Destroy before DeviceResources destroys the VMA allocator and before vkDestroyDevice.
class DescriptorHeapTable {
public:
    DescriptorHeapTable() noexcept = default;
    ~DescriptorHeapTable();

    DescriptorHeapTable(const DescriptorHeapTable&) = delete;
    DescriptorHeapTable& operator=(const DescriptorHeapTable&) = delete;

    [[nodiscard]] std::expected<void, core::Error> open(
        VkPhysicalDevice physical,
        VkDevice device,
        DeviceResources& resources,
        const DescriptorHeapLimits& limits,
        const DescriptorHeapDispatch& dispatch,
        DescriptorHeapCapacity capacity);

    [[nodiscard]] std::expected<void, core::Error>
    writeImageSlot(std::uint32_t slot, VkImage image, VkFormat format);
    [[nodiscard]] std::expected<void, core::Error>
    writeBufferSlot(std::uint32_t slot, VkDeviceAddress address, VkDeviceSize size);
    [[nodiscard]] std::expected<void, core::Error>
    writeSamplerSlot(std::uint32_t slot, const VkSamplerCreateInfo& sampler);

    [[nodiscard]] VkBindHeapInfoEXT resourceBindInfo() const noexcept;
    [[nodiscard]] VkBindHeapInfoEXT samplerBindInfo() const noexcept;

    [[nodiscard]] VkDeviceAddress resourceHeapAddress() const noexcept {
        return m_resourceAddress;
    }
    [[nodiscard]] VkDeviceAddress samplerHeapAddress() const noexcept {
        return m_samplerAddress;
    }
    [[nodiscard]] const DescriptorHeapPlan& plan() const noexcept {
        return m_plan;
    }
    [[nodiscard]] const DescriptorHeapDispatch& dispatch() const noexcept {
        return m_dispatch;
    }

    void close() noexcept;
    void abandon() noexcept {
        m_resources = nullptr;
        m_resourceBuffer = VK_NULL_HANDLE;
        m_resourceAllocation = nullptr;
        m_samplerBuffer = VK_NULL_HANDLE;
        m_samplerAllocation = nullptr;
    }

private:
    std::expected<void, core::Error> allocateHeap(
        VkDeviceSize size,
        VkDeviceSize alignment,
        PFN_vkGetBufferDeviceAddress getDeviceAddress,
        const char* label,
        VkBuffer& outBuffer,
        VmaAllocation_T*& outAllocation,
        std::byte*& outMapped,
        VkDeviceAddress& outAddress);

    VkDevice m_device = VK_NULL_HANDLE;
    DeviceResources* m_resources = nullptr;
    DescriptorHeapLimits m_limits{};
    DescriptorHeapDispatch m_dispatch{};
    DescriptorHeapCapacity m_capacity{};
    DescriptorHeapPlan m_plan{};

    VkBuffer m_resourceBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_resourceAllocation = nullptr;
    std::byte* m_resourceMapped = nullptr;
    VkDeviceAddress m_resourceAddress = 0;

    VkBuffer m_samplerBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_samplerAllocation = nullptr;
    std::byte* m_samplerMapped = nullptr;
    VkDeviceAddress m_samplerAddress = 0;
};

} // namespace ar::rhi::detail
