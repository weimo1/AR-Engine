#include "Presentation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "PresentationUtil.h"
#include "binding/DescriptorHeapTable.h"
#include "rhi/RhiSession.h"

namespace ar::rhi::detail {

using presentation::create;
using presentation::failed;
using presentation::toVkFormat;

std::expected<void, core::Error> Presentation::createLightingLayout() {
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (auto created =
            create(m_device, m_lightingSampler, sampler, vkCreateSampler, "lighting sampler");
        !created) {
        return created;
    }
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    const std::uint32_t bindingCount = m_lighting->environmentLighting ? 7u : 4u;
    if (m_lighting->environmentLighting) {
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.maxLod = 4.F;
        if (auto created = create(
                m_device, m_environmentSampler, sampler, vkCreateSampler, "environment sampler");
            !created) {
            return created;
        }
    }
    for (std::uint32_t i = 0; i < bindingCount; ++i) {
        bindings[i] = {
            i,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            1,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            i < 4 ? &m_lightingSampler : &m_environmentSampler};
    }
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = bindingCount;
    set.pBindings = bindings.data();
    if (auto created = create(
            m_device,
            m_lightingSetLayout,
            set,
            vkCreateDescriptorSetLayout,
            "lighting descriptor layout");
        !created) {
        return created;
    }
    const VkPushConstantRange push{
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        static_cast<std::uint32_t>(
            sizeof(m_frameView.inverseProjection) + sizeof(m_frameView.viewToWorld))};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m_lightingSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    return create(
        m_device, m_lightingLayout, layout, vkCreatePipelineLayout, "lighting pipeline layout");
}

std::expected<void, core::Error> Presentation::createLightingResources(VkImageUsageFlags usage) {
    constexpr std::uint32_t setsPerSlot = 2;
    constexpr std::uint32_t gbufferBindings = 4;
    const std::uint32_t bindingsPerSet = m_lighting->environmentLighting ? 7u : gbufferBindings;
    const auto setCount = setsPerSlot * static_cast<std::uint32_t>(m_swapchain.attachments.size());
    const VkDescriptorPoolSize size{
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount * bindingsPerSet};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = setCount;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (auto created = create(
            m_device,
            m_swapchain.lightingPool,
            pool,
            vkCreateDescriptorPool,
            "lighting descriptor pool");
        !created) {
        return created;
    }

    const VkFormat hdrFormat = toVkFormat(m_spec.hdrFormat);
    for (auto& slot : m_swapchain.attachments) {
        auto attachment = m_staticScene->createAttachment(
            m_swapchain.extent, hdrFormat, VK_IMAGE_ASPECT_COLOR_BIT, usage);
        if (!attachment) {
            return std::unexpected(attachment.error());
        }
        slot.hdr = *attachment;
        const std::array layouts{m_lightingSetLayout, m_lightingSetLayout};
        std::array<VkDescriptorSet, setsPerSlot> sets{};
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = m_swapchain.lightingPool;
        allocate.descriptorSetCount = setsPerSlot;
        allocate.pSetLayouts = layouts.data();
        const VkResult result = vkAllocateDescriptorSets(m_device, &allocate, sets.data());
        if (result != VK_SUCCESS) {
            return std::unexpected(failed("allocating lighting descriptor sets", result));
        }
        slot.directSet = sets[0];
        slot.displaySet = sets[1];
        const std::array views{
            slot.gbuffer[0].view,
            slot.gbuffer[1].view,
            slot.gbuffer[2].view,
            slot.depth.view,
            slot.hdr.view};
        std::array<VkDescriptorImageInfo, views.size()> images{};
        std::array<VkWriteDescriptorSet, views.size()> writes{};
        for (std::uint32_t i = 0; i < views.size(); ++i) {
            images[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            auto& write = writes[i];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = i < gbufferBindings ? slot.directSet : slot.displaySet;
            write.dstBinding = i < gbufferBindings ? i : 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(
            m_device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    if (m_environmentReady) {
        writeEnvironmentDescriptors();
    }
    return {};
}

void Presentation::writeEnvironmentDescriptors() const {
    for (const auto& slot : m_swapchain.attachments) {
        std::array<VkDescriptorImageInfo, 3> images{};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            images[i] = {
                VK_NULL_HANDLE, m_environment[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[i] = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                slot.directSet,
                4u + i,
                0,
                1,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                &images[i],
                nullptr,
                nullptr};
        }
        vkUpdateDescriptorSets(
            m_device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void Presentation::recordLighting(
    VkCommandBuffer commands, const Image& output, const PresentationGraphPlan& plan) const {
    const auto& slot = m_swapchain.attachments[m_frameIndex];
    recordGraphBarriers(commands, output, plan, PresentationPass::Lighting);

    const auto draw = [&](VkImageView target,
                          VkPipeline pipeline,
                          VkDescriptorSet set,
                          std::uint32_t size,
                          const void* push) {
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = target;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};
        render.renderArea.extent = m_swapchain.extent;
        render.layerCount = 1;
        render.colorAttachmentCount = 1;
        render.pColorAttachments = &color;
        vkCmdBeginRendering(commands, &render);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(
            commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(commands, m_lightingLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, push);
        vkCmdDraw(commands, 3, 1, 0, 0);
        vkCmdEndRendering(commands);
    };

    auto inverse = m_frameView.inverseProjection;
    inverse[0] *= static_cast<float>(m_swapchain.extent.width) /
                  static_cast<float>(m_swapchain.extent.height);
    const struct DirectPush {
        std::array<float, 16> inverseProjection;
        std::array<float, 12> viewToWorld;
    } direct{inverse, m_frameView.viewToWorld};
    static_assert(sizeof(DirectPush) == 112);
    draw(slot.hdr.view, m_swapchain.directPipeline, slot.directSet, sizeof(direct), &direct);

    recordGraphBarriers(commands, output, plan, PresentationPass::Display);

    const bool srgb = m_swapchain.format == VK_FORMAT_B8G8R8A8_SRGB ||
                      m_swapchain.format == VK_FORMAT_R8G8B8A8_SRGB;
    const DisplayPushData display = m_spec.buildDisplayPush(m_spec.displayExposureEV, srgb);
    draw(
        output.view,
        m_swapchain.displayPipeline,
        slot.displaySet,
        display.size,
        display.bytes.data());
}

std::expected<void, core::Error> Presentation::createGraphicsPipeline(
    VkFormat format,
    const std::vector<std::uint32_t>& vertexCode,
    const std::vector<std::uint32_t>& fragmentCode,
    VkPipeline& output,
    bool fullscreen) {
    const bool geometry = m_staticScene && !fullscreen;
    const bool gbuffer = m_enableGBuffer && !fullscreen;
    struct ShaderModules {
        VkDevice device;
        std::array<VkShaderModule, 2> handles{};
        ~ShaderModules() {
            for (auto handle : handles) {
                if (handle) {
                    vkDestroyShaderModule(device, handle, nullptr);
                }
            }
        }
    } modules{m_device};

    const std::array bytecode{&vertexCode, &fragmentCode};
    const std::array stages{VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    std::array<VkPipelineShaderStageCreateInfo, 2> stageInfos{};
    for (std::size_t i = 0; i < stageInfos.size(); ++i) {
        VkShaderModuleCreateInfo module{};
        module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module.codeSize = bytecode[i]->size() * sizeof(std::uint32_t);
        module.pCode = bytecode[i]->data();
        if (auto created = create(
                m_device, modules.handles[i], module, vkCreateShaderModule, "vkCreateShaderModule");
            !created) {
            return created;
        }
        stageInfos[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfos[i].stage = stages[i];
        stageInfos[i].module = modules.handles[i];
        stageInfos[i].pName = "main";
    }

    VkPipelineVertexInputStateCreateInfo vertex{};
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    const VkVertexInputBindingDescription positions{
        0, 3 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription position{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    const VkVertexInputBindingDescription meshBinding{
        0, sizeof(StaticVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array meshAttributes{
        VkVertexInputAttributeDescription{
            0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(StaticVertex, position)},
        VkVertexInputAttributeDescription{
            1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(StaticVertex, uv)},
        VkVertexInputAttributeDescription{
            2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(StaticVertex, normal)}};
    if (m_cullFrames.front() && !fullscreen) {
        vertex.vertexBindingDescriptionCount = 1;
        vertex.pVertexBindingDescriptions = &positions;
        vertex.vertexAttributeDescriptionCount = 1;
        vertex.pVertexAttributeDescriptions = &position;
    }
    if (geometry) {
        vertex.vertexBindingDescriptionCount = 1;
        vertex.pVertexBindingDescriptions = &meshBinding;
        vertex.vertexAttributeDescriptionCount = gbuffer ? 3 : 2;
        vertex.pVertexAttributeDescriptions = meshAttributes.data();
    }
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const float height = static_cast<float>(m_swapchain.extent.height);
    const VkViewport viewport{
        0.0F,
        gbuffer ? height : 0.F,
        static_cast<float>(m_swapchain.extent.width),
        gbuffer ? -height : height,
        0.0F,
        1.0F};
    const VkRect2D scissor{{0, 0}, m_swapchain.extent};
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState color{};
    color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    const std::array colors{color, color, color, color};
    blend.attachmentCount = gbuffer ? static_cast<std::uint32_t>(colors.size()) : 1;
    blend.pAttachments = colors.data();
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    const std::array formats{
        format,
        toVkFormat(m_spec.gbufferFormats[0]),
        toVkFormat(m_spec.gbufferFormats[1]),
        toVkFormat(m_spec.gbufferFormats[2])};
    rendering.colorAttachmentCount = blend.attachmentCount;
    rendering.pColorAttachmentFormats = formats.data();
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (geometry) {
        rendering.depthAttachmentFormat = toVkFormat(m_spec.depthFormat);
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = m_spec.depthReverseZ ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS;
    }
    VkPipelineCreateFlags2CreateInfo flags2{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};
    std::array<VkDescriptorSetAndBindingMappingEXT, 2> heapMappings{};
    VkShaderDescriptorSetAndBindingMappingInfoEXT heapMappingInfo{
        VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT};
    if (m_heapBinding) {
        const auto& plan = m_heapBinding->plan();
        heapMappings[0].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
        heapMappings[0].descriptorSet = 0;
        heapMappings[0].firstBinding = 0;
        heapMappings[0].bindingCount = 1;
        heapMappings[0].resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
        heapMappings[0].source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
        heapMappings[0].sourceData.constantOffset.heapOffset = 0;
        heapMappings[0].sourceData.constantOffset.heapArrayStride =
            static_cast<std::uint32_t>(plan.imageStride);

        heapMappings[1].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
        heapMappings[1].descriptorSet = 0;
        heapMappings[1].firstBinding = 1;
        heapMappings[1].bindingCount = 1;
        heapMappings[1].resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
        heapMappings[1].source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
        heapMappings[1].sourceData.constantOffset.heapOffset = 0;
        heapMappings[1].sourceData.constantOffset.heapArrayStride = 0;

        heapMappingInfo.mappingCount = static_cast<std::uint32_t>(heapMappings.size());
        heapMappingInfo.pMappings = heapMappings.data();
        stageInfos[1].pNext = &heapMappingInfo;

        flags2.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
        flags2.pNext = &rendering;
    }
    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.pNext =
        m_heapBinding ? static_cast<const void*>(&flags2) : static_cast<const void*>(&rendering);
    pipeline.stageCount = static_cast<std::uint32_t>(stageInfos.size());
    pipeline.pStages = stageInfos.data();
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewportState;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    if (geometry) {
        pipeline.pDepthStencilState = &depth;
    }
    pipeline.layout =
        m_heapBinding ? VK_NULL_HANDLE : (fullscreen ? m_lightingLayout : m_pipelineLayout);
    const VkResult result =
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &output);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateGraphicsPipelines", result));
    }
    return {};
}

} // namespace ar::rhi::detail
