#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include <volk.h>

#include "binding/DescriptorHeapLayout.h"
#include "binding/SetBindingLimits.h"
#include "binding/SetBindingSlots.h"
#include "core/Error.h"
#include "rhi/Image.h"

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

// Owns aligned VMA-backed Resource Heap and Sampler Heap buffers plus their slot tables for
// VK_EXT_descriptor_heap. Retains sampled image references in DeviceResources until retired slots
// finish GPU execution and are overwritten with slot 0 defaults (or on close()).
// Destroy before DeviceResources destroys the VMA allocator and before vkDestroyDevice.
class DescriptorHeapTable {
    struct HeapImageDescriptor {
        ImageHandle handle{};
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    };

    using ImageSlots = SetBindingSlots<HeapImageDescriptor, kStaticSceneImageLimit>;
    using SamplerSlots = SetBindingSlots<VkSamplerCreateInfo, kSetSamplerCount>;

public:
    using ImageBinding = ImageSlots::Handle;
    using SamplerBinding = SamplerSlots::Handle;

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
        DescriptorHeapCapacity capacity,
        ImageHandle defaultImage,
        VkFormat defaultImageFormat,
        const VkSamplerCreateInfo& defaultSampler);

    [[nodiscard]] std::expected<ImageBinding, core::Error>
    registerImage(ImageHandle image, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
    [[nodiscard]] std::optional<std::uint32_t> resolveImage(ImageBinding handle) const noexcept;
    bool retireImage(ImageBinding handle) noexcept;

    [[nodiscard]] std::expected<SamplerBinding, core::Error>
    registerSampler(const VkSamplerCreateInfo& sampler);
    [[nodiscard]] std::optional<std::uint32_t> resolveSampler(SamplerBinding handle) const noexcept;
    bool retireSampler(SamplerBinding handle) noexcept;

    [[nodiscard]] bool hasPendingChanges() const noexcept {
        return (m_imageSlots && m_imageSlots->hasChanges()) ||
               (m_samplerSlots && m_samplerSlots->hasChanges());
    }
    [[nodiscard]] std::expected<bool, core::Error> publishCompleted(bool submissionsComplete);

    [[nodiscard]] VkImage imageForSlot(std::uint32_t slot) const noexcept {
        return m_imageSlots ? m_imageSlots->descriptor(slot).image : VK_NULL_HANDLE;
    }

    [[nodiscard]] std::expected<void, core::Error>
    writeBufferSlot(std::uint32_t slot, VkDeviceAddress address, VkDeviceSize size);

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
        m_imageSlots.reset();
        m_samplerSlots.reset();
        m_device = VK_NULL_HANDLE;
        m_resources = nullptr;
        m_resourceBuffer = VK_NULL_HANDLE;
        m_resourceAllocation = nullptr;
        m_resourceMapped = nullptr;
        m_resourceAddress = 0;
        m_samplerBuffer = VK_NULL_HANDLE;
        m_samplerAllocation = nullptr;
        m_samplerMapped = nullptr;
        m_samplerAddress = 0;
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
    [[nodiscard]] std::expected<void, core::Error>
    writeImageSlot(std::uint32_t slot, VkImage image, VkFormat format);
    [[nodiscard]] std::expected<void, core::Error>
    writeSamplerSlot(std::uint32_t slot, const VkSamplerCreateInfo& sampler);

    VkDevice m_device = VK_NULL_HANDLE;
    DeviceResources* m_resources = nullptr;
    DescriptorHeapLimits m_limits{};
    DescriptorHeapDispatch m_dispatch{};
    DescriptorHeapCapacity m_capacity{};
    DescriptorHeapPlan m_plan{};

    std::optional<ImageSlots> m_imageSlots;
    std::optional<SamplerSlots> m_samplerSlots;

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
