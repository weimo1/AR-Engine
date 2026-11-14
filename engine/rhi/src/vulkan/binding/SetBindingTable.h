#pragma once

#include <expected>

#include <volk.h>

#include "SetBindingLimits.h"
#include "SetBindingSlots.h"
#include "core/Error.h"

namespace ar::rhi::detail {

class DeviceResources;

// Borrows the device and the default view owned by DeviceResources.
class SetBindingTable {
    using ImageSlots = SetBindingSlots<VkDescriptorImageInfo, kSetImageCount>;
    using SamplerSlots = SetBindingSlots<VkDescriptorImageInfo, kSetSamplerCount>;

public:
    SetBindingTable(VkDevice device, DeviceResources& resources) noexcept
        : m_device(device), m_resources(resources) {}
    ~SetBindingTable();

    SetBindingTable(const SetBindingTable&) = delete;
    SetBindingTable& operator=(const SetBindingTable&) = delete;

    std::expected<void, core::Error> open(VkImageView defaultView);
    std::expected<void, core::Error>
    populateSample(VkImageView stripes, VkImageView blue, VkImageView secondView);
    using ImageBinding = ImageSlots::Handle;
    [[nodiscard]] std::optional<ImageBinding> registerImage(VkImageView view);
    [[nodiscard]] std::optional<std::size_t> resolveImage(ImageBinding handle) const noexcept;
    bool retireImage(ImageBinding handle);
    [[nodiscard]] const auto& sampleImages() const noexcept {
        return m_sampleImages;
    }
    [[nodiscard]] bool hasChanges() const noexcept {
        return m_images->hasChanges() || m_samplers->hasChanges();
    }
    bool publishCompleted(bool firstFrameComplete, bool secondFrameComplete);

    [[nodiscard]] VkDescriptorSetLayout layout() const noexcept {
        return m_layout;
    }
    [[nodiscard]] VkDescriptorSet set() const noexcept {
        return m_set;
    }
    void abandon() noexcept {
        m_device = VK_NULL_HANDLE;
    }

private:
    void writeDescriptors();
    std::array<std::optional<ImageBinding>, 3> m_sampleImages{};
    std::optional<ImageSlots> m_images;
    std::optional<SamplerSlots> m_samplers;
    VkDevice m_device;
    DeviceResources& m_resources;
    VkSampler m_linearSampler = VK_NULL_HANDLE;
    VkSampler m_defaultSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace ar::rhi::detail
