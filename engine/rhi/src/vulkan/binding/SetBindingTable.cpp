#include "SetBindingTable.h"

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
    std::array<VkDescriptorImageInfo, kSetImageCount> images;
    std::array<VkDescriptorImageInfo, kSetSamplerCount> samplers;
    for (std::size_t index = 0; index < images.size(); ++index) {
        images[index] = m_images->descriptor(index);
    }
    for (std::size_t index = 0; index < samplers.size(); ++index) {
        samplers[index] = m_samplers->descriptor(index);
    }
    std::array<VkWriteDescriptorSet, bindings.size()> writes{};
    const std::array<const VkDescriptorImageInfo*, bindings.size()> descriptors{
        images.data(), samplers.data()};
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        auto& write = writes[index];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = bindings[index].binding;
        write.descriptorCount = bindings[index].descriptorCount;
        write.descriptorType = bindings[index].descriptorType;
        write.pImageInfo = descriptors[index];
    }
    vkUpdateDescriptorSets(
        m_device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return {};
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
    const std::array images{
        VkDescriptorImageInfo{VK_NULL_HANDLE, stripes, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{VK_NULL_HANDLE, blue, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{VK_NULL_HANDLE, secondView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    const VkDescriptorImageInfo linear{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = writes[1].dstSet = m_set;
    writes[0].dstArrayElement = writes[1].dstArrayElement = 1;
    writes[0].descriptorCount = static_cast<std::uint32_t>(images.size());
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = images.data();
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = &linear;
    vkUpdateDescriptorSets(m_device, 2, writes.data(), 0, nullptr);
    return {};
}

} // namespace ar::rhi::detail
