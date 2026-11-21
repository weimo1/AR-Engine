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

std::expected<void, core::Error>
SetBindingTable::open(VkImageView defaultView, std::uint32_t imageCount, bool repeat) {
    if (imageCount == 0 || imageCount > kStaticSceneImageLimit) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Validation,
                "invalid sampled image table capacity",
                {}});
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    const auto address =
        repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeU = address;
    sampler.addressModeV = address;
    sampler.addressModeW = address;
    VkSampler samplerHandle = VK_NULL_HANDLE;
    if (const auto result = vkCreateSampler(m_device, &sampler, nullptr, &samplerHandle);
        result != VK_SUCCESS) {
        return std::unexpected(bindingError("vkCreateSampler", result));
    }
    m_defaultSampler = samplerHandle;

    const std::array bindings{
        VkDescriptorSetLayoutBinding{
            0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, imageCount, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
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

    const std::array sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, imageCount},
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

    const VkDescriptorImageInfo imageInfo{
        VK_NULL_HANDLE, defaultView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo samplerInfo{
        m_defaultSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    m_images.emplace(imageInfo, imageCount);
    m_samplers.emplace(samplerInfo);
    std::array<VkDescriptorImageInfo, kStaticSceneImageLimit> images;
    std::array<VkDescriptorImageInfo, kSetSamplerCount> samplers;
    images.fill(imageInfo);
    samplers.fill(samplerInfo);
    writeDescriptors(0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, std::span(images).first(imageCount));
    writeDescriptors(1, 0, VK_DESCRIPTOR_TYPE_SAMPLER, samplers);
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
    const bool imagesChanged = m_images->applyCompleted(
        firstFrameComplete,
        secondFrameComplete,
        [this](std::size_t index, const auto& descriptor) {
            writeDescriptors(0, index, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, {&descriptor, 1});
        },
        [this](const auto& descriptor) { m_resources.releaseSampledView(descriptor.imageView); });
    const bool samplersChanged = m_samplers->applyCompleted(
        firstFrameComplete,
        secondFrameComplete,
        [this](std::size_t index, const auto& descriptor) {
            writeDescriptors(1, index, VK_DESCRIPTOR_TYPE_SAMPLER, {&descriptor, 1});
        },
        [](const auto&) {});
    return imagesChanged || samplersChanged;
}

void SetBindingTable::writeDescriptors(
    std::uint32_t binding,
    std::size_t index,
    VkDescriptorType type,
    std::span<const VkDescriptorImageInfo> descriptors) {
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_set;
    write.dstBinding = binding;
    write.dstArrayElement = static_cast<std::uint32_t>(index);
    write.descriptorCount = static_cast<std::uint32_t>(descriptors.size());
    write.descriptorType = type;
    write.pImageInfo = descriptors.data();
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
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
