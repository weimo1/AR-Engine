#include "SetBindingTable.h"
#include "resources/DeviceResources.h"

#include <array>
#include <string>

#include "SetBindingLimits.h"
#include "VulkanResult.h"

namespace ar::rhi::detail {
namespace {

core::Error bindingError(const char* operation, VkResult result) {
    return {
        core::ErrorCategory::InitializationFailed,
        core::ErrorStage::Capability,
        std::string(operation) + " returned " + vkResultName(result),
        core::ErrorOrigin{"vulkan", result}};
}

} // namespace

SetBindingTable::~SetBindingTable() {
    if (m_device) {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
        vkDestroySampler(m_device, m_linearSampler, nullptr);
        vkDestroySampler(m_device, m_defaultSampler, nullptr);
    }
}

std::expected<void, core::Error> SetBindingTable::open(VkImageView defaultView) {
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler samplerHandle = VK_NULL_HANDLE;
    if (const auto result = vkCreateSampler(m_device, &sampler, nullptr, &samplerHandle);
        result != VK_SUCCESS) {
        return std::unexpected(bindingError("vkCreateSampler", result));
    }
    m_defaultSampler = samplerHandle;

    constexpr std::array bindings{
        VkDescriptorSetLayoutBinding{
            0,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            kSetImageCount,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            nullptr},
        VkDescriptorSetLayoutBinding{
            1, VK_DESCRIPTOR_TYPE_SAMPLER, kSetSamplerCount, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    VkDescriptorSetLayout layoutHandle = VK_NULL_HANDLE;
    if (const auto result = vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &layoutHandle);
        result != VK_SUCCESS) {
        return std::unexpected(bindingError("vkCreateDescriptorSetLayout", result));
    }
    m_layout = layoutHandle;

    constexpr std::array sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetImageCount},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, kSetSamplerCount},
    };
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    VkDescriptorPool poolHandle = VK_NULL_HANDLE;
    if (const auto result = vkCreateDescriptorPool(m_device, &pool, nullptr, &poolHandle);
        result != VK_SUCCESS) {
        return std::unexpected(bindingError("vkCreateDescriptorPool", result));
    }
    m_pool = poolHandle;

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = m_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &m_layout;
    if (const auto result = vkAllocateDescriptorSets(m_device, &allocate, &m_set);
        result != VK_SUCCESS) {
        return std::unexpected(bindingError("vkAllocateDescriptorSets", result));
    }

    m_images.emplace(
        VkDescriptorImageInfo{
            VK_NULL_HANDLE, defaultView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    m_samplers.emplace(
        VkDescriptorImageInfo{m_defaultSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    writeDescriptors();
    return {};
}

std::optional<SetBindingTable::ImageBinding> SetBindingTable::registerImage(VkImageView view) {
    return m_images->registerPending(
        {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
}

std::optional<std::size_t> SetBindingTable::resolveImage(ImageBinding handle) const noexcept {
    return m_images->resolve(handle);
}

bool SetBindingTable::retireImage(ImageBinding handle) {
    return m_images->retire(handle, [this](const auto& descriptor) {
        m_resources.releaseSampledView(descriptor.imageView);
    });
}

bool SetBindingTable::publishCompleted(bool firstFrameComplete, bool secondFrameComplete) {
    const bool samplersChanged =
        m_samplers->applyCompleted(firstFrameComplete, secondFrameComplete);
    const bool imagesChanged = m_images->applyCompleted(
        firstFrameComplete,
        secondFrameComplete,
        [this] { writeDescriptors(); },
        [this](const auto& descriptor) { m_resources.releaseSampledView(descriptor.imageView); });
    if (samplersChanged && !imagesChanged) {
        writeDescriptors();
    }
    return imagesChanged || samplersChanged;
}

void SetBindingTable::writeDescriptors() {
    std::array<VkDescriptorImageInfo, kSetImageCount> images;
    std::array<VkDescriptorImageInfo, kSetSamplerCount> samplers;
    for (std::size_t index = 0; index < images.size(); ++index) {
        images[index] = m_images->descriptor(index);
    }
    for (std::size_t index = 0; index < samplers.size(); ++index) {
        samplers[index] = m_samplers->descriptor(index);
    }
    std::array<VkWriteDescriptorSet, 2> writes{};
    constexpr std::array bindings{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    constexpr std::array counts{kSetImageCount, kSetSamplerCount};
    const std::array<const VkDescriptorImageInfo*, bindings.size()> descriptors{
        images.data(), samplers.data()};
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        auto& write = writes[index];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = static_cast<std::uint32_t>(index);
        write.descriptorCount = counts[index];
        write.descriptorType = bindings[index];
        write.pImageInfo = descriptors[index];
    }
    vkUpdateDescriptorSets(
        m_device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

std::expected<void, core::Error>
SetBindingTable::populateSample(VkImageView stripes, VkImageView blue, VkImageView secondView) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (const auto result = vkCreateSampler(m_device, &info, nullptr, &sampler);
        result != VK_SUCCESS) {
        return std::unexpected(bindingError("vkCreateSampler", result));
    }
    m_linearSampler = sampler;
    m_sampleImages = {registerImage(stripes), registerImage(blue), registerImage(secondView)};
    if (!m_sampleImages[0] || !m_sampleImages[1] || !m_sampleImages[2] ||
        !m_samplers->registerPending({sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED})) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "sample descriptor table capacity exhausted",
                std::nullopt});
    }
    // Initialization runs before the first frame can reference the set.
    publishCompleted(true, true);
    return {};
}

} // namespace ar::rhi::detail
