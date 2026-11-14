#pragma once

#include <expected>

#include <volk.h>

#include "SetBindingLimits.h"
#include "SetBindingSlots.h"
#include "core/Error.h"

namespace ar::rhi::detail {

// Borrows the device and the default view owned by DeviceResources.
class SetBindingTable {
public:
    explicit SetBindingTable(VkDevice device) noexcept : m_device(device) {}
    ~SetBindingTable();

    SetBindingTable(const SetBindingTable&) = delete;
    SetBindingTable& operator=(const SetBindingTable&) = delete;

    std::expected<void, core::Error> open(VkImageView defaultView);
    void abandon() noexcept {
        m_device = VK_NULL_HANDLE;
    }

private:
    std::optional<SetBindingSlots<VkDescriptorImageInfo, kSetImageCount>> m_images;
    std::optional<SetBindingSlots<VkDescriptorImageInfo, kSetSamplerCount>> m_samplers;
    VkDevice m_device;
    VkSampler m_defaultSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace ar::rhi::detail
