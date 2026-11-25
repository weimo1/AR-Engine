#include "Presentation.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "PresentationUtil.h"
#include "binding/DescriptorHeapTable.h"
#include "rhi/RhiSession.h"

namespace ar::rhi::detail {

using presentation::create;
using presentation::failed;
using presentation::makeShadowCompareSamplerInfo;
using presentation::toVkFormat;

std::expected<void, core::Error> Presentation::createLightingLayout() {
    static_assert(sizeof(LightingFrameGpu) == 240);
    static_assert(offsetof(LightingFrameGpu, inverseProjection) == 0);
    static_assert(offsetof(LightingFrameGpu, viewToWorld) == 64);
    static_assert(offsetof(LightingFrameGpu, lightViewProj) == 128);
    static_assert(offsetof(LightingFrameGpu, lightDirView) == 192);
    static_assert(offsetof(LightingFrameGpu, lightColor) == 208);
    static_assert(offsetof(LightingFrameGpu, shadowParams) == 224);

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
    if (m_lighting->shadow) {
        const VkSamplerCreateInfo shadowSampler = makeShadowCompareSamplerInfo(m_physical);
        if (auto created =
                create(m_device, m_shadowSampler, shadowSampler, vkCreateSampler, "shadow sampler");
            !created) {
            return created;
        }
    }

    std::array<VkDescriptorSetLayoutBinding, 9> bindings{};
    std::uint32_t bindingCount = 0;
    for (std::uint32_t i = 0; i < 4u; ++i) {
        bindings[bindingCount++] = {
            i,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            1,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            &m_lightingSampler};
    }
    if (m_lighting->environmentLighting) {
        for (std::uint32_t i = 0; i < 3u; ++i) {
            bindings[bindingCount++] = {
                4u + i,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                1,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                &m_environmentSampler};
        }
    }
    if (m_lighting->shadow) {
        bindings[bindingCount++] = {
            7u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            1,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            &m_shadowSampler};
    }
    bindings[bindingCount++] = {
        8u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

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
    VkPipelineLayoutCreateInfo lightingLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lightingLayout.setLayoutCount = 1;
    lightingLayout.pSetLayouts = &m_lightingSetLayout;
    if (auto created = create(
            m_device,
            m_lightingLayout,
            lightingLayout,
            vkCreatePipelineLayout,
            "lighting pipeline layout");
        !created) {
        return created;
    }

    std::array<VkDescriptorSetLayoutBinding, 6> displayBindings{};
    for (std::uint32_t i = 0; i < displayBindings.size(); ++i) {
        displayBindings[i] = {
            i,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            1,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            &m_lightingSampler};
    }
    VkDescriptorSetLayoutCreateInfo displaySet{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    displaySet.bindingCount = static_cast<std::uint32_t>(displayBindings.size());
    displaySet.pBindings = displayBindings.data();
    if (auto created = create(
            m_device,
            m_displaySetLayout,
            displaySet,
            vkCreateDescriptorSetLayout,
            "display descriptor layout");
        !created) {
        return created;
    }
    const VkPushConstantRange displayPush{
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        static_cast<std::uint32_t>(sizeof(DisplayPushData::bytes))};
    VkPipelineLayoutCreateInfo displayLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    displayLayout.setLayoutCount = 1;
    displayLayout.pSetLayouts = &m_displaySetLayout;
    displayLayout.pushConstantRangeCount = 1;
    displayLayout.pPushConstantRanges = &displayPush;
    return create(
        m_device,
        m_displayLayout,
        displayLayout,
        vkCreatePipelineLayout,
        "display pipeline layout");
}

std::expected<void, core::Error> Presentation::writeShadowHeapDescriptors() {
    if (!m_heapBinding || !m_lighting || !m_lighting->shadow) {
        return {};
    }
    for (std::uint32_t frame = 0; frame < kHeapShadowImageCount; ++frame) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = m_shadowDepth[frame].image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_D32_SFLOAT;
        view.components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY};
        view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (auto written =
                m_heapBinding->writeExternalImageSlot(kHeapShadowImageBase + frame, view);
            !written) {
            return written;
        }
    }
    return {};
}

void Presentation::resetShadowHeapDescriptors() noexcept {
    if (!m_heapBinding) {
        return;
    }
    for (std::uint32_t frame = 0; frame < kHeapShadowImageCount; ++frame) {
        static_cast<void>(
            m_heapBinding->resetExternalImageSlotToDefault(kHeapShadowImageBase + frame));
    }
}

std::expected<void, core::Error> Presentation::createShadowPipeline(
    bool indirect,
    const std::vector<std::uint32_t>& vertexCode,
    VkPipelineLayout layout,
    VkPipeline& output) {
    struct ShaderModule {
        VkDevice device;
        VkShaderModule handle = VK_NULL_HANDLE;
        ~ShaderModule() {
            if (handle) {
                vkDestroyShaderModule(device, handle, nullptr);
            }
        }
    } module{m_device};

    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = vertexCode.size() * sizeof(std::uint32_t);
    moduleInfo.pCode = vertexCode.data();
    if (auto created = create(
            m_device, module.handle, moduleInfo, vkCreateShaderModule, "shadow shader module");
        !created) {
        return created;
    }

    VkPipelineShaderStageCreateInfo stageInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stageInfo.module = module.handle;
    stageInfo.pName = "main";

    VkDescriptorSetAndBindingMappingEXT indirectMapping{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT};
    VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{
        VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT};
    VkPipelineCreateFlags2CreateInfo flags2{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};

    const VkVertexInputBindingDescription binding{
        0, sizeof(StaticVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription position{
        0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(StaticVertex, position)};
    VkPipelineVertexInputStateCreateInfo vertex{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &binding;
    vertex.vertexAttributeDescriptionCount = 1;
    vertex.pVertexAttributeDescriptions = &position;

    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    const std::uint32_t resolution = m_lighting->shadow->resolution;
    const float extent = static_cast<float>(resolution);
    const VkViewport viewport{0.F, 0.F, extent, extent, 0.F, 1.F};
    const VkRect2D scissor{{0, 0}, {resolution, resolution}};
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.depthBiasEnable = VK_TRUE;
    raster.depthBiasConstantFactor = 1.5F;
    raster.depthBiasClamp = 0.0F;
    raster.depthBiasSlopeFactor = 2.0F;
    raster.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 0;
    blend.pAttachments = nullptr;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 0;
    rendering.pColorAttachmentFormats = nullptr;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    if (m_heapBinding) {
        if (indirect) {
            const auto& plan = m_heapBinding->plan();
            indirectMapping.descriptorSet = 0u;
            indirectMapping.firstBinding = 0u;
            indirectMapping.bindingCount = 1u;
            indirectMapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT;
            indirectMapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
            indirectMapping.sourceData.constantOffset.heapOffset =
                static_cast<std::uint32_t>(plan.bufferBaseOffset);
            indirectMapping.sourceData.constantOffset.heapArrayStride = 0u;
            mappingInfo.mappingCount = 1u;
            mappingInfo.pMappings = &indirectMapping;
            stageInfo.pNext = &mappingInfo;
        }
        flags2.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
        flags2.pNext = &rendering;
    }

    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.pNext =
        m_heapBinding ? static_cast<const void*>(&flags2) : static_cast<const void*>(&rendering);
    pipeline.stageCount = 1u;
    pipeline.pStages = &stageInfo;
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewportState;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.layout = m_heapBinding ? VK_NULL_HANDLE : layout;
    const VkResult result =
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &output);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateGraphicsPipelines(shadow)", result));
    }
    return {};
}

std::expected<void, core::Error> Presentation::createPersistentLightingResources() {
    const LightingFrameGpu initialFrame{};
    for (std::uint32_t frame = 0; frame < kHeapLightingBufferCount; ++frame) {
        auto ubo = m_staticScene->createUniformBuffer(sizeof(LightingFrameGpu));
        if (!ubo) {
            return std::unexpected(std::move(ubo.error()));
        }
        m_lightingFrames[frame] = *ubo;
        if (auto written = m_staticScene->writeUniformBuffer(
                m_lightingFrames[frame], std::as_bytes(std::span{&initialFrame, 1}));
            !written) {
            return written;
        }
        if (m_heapBinding) {
            if (auto bound = m_heapBinding->writeBufferSlot(
                    kHeapLightingBufferBase + frame,
                    m_lightingFrames[frame].address,
                    VkDeviceSize{sizeof(LightingFrameGpu)},
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                !bound) {
                return bound;
            }
        }
    }

    if (!m_lighting->shadow) {
        return {};
    }

    const auto& shadow = *m_lighting->shadow;
    m_lightingState.normalBias = shadow.normalBias;
    m_lightingState.depthBias = shadow.depthBias;
    m_lightingState.pcfRadiusTexels = shadow.pcfRadiusTexels;

    const VkExtent2D shadowExtent{shadow.resolution, shadow.resolution};
    for (std::size_t frame = 0; frame < m_shadowDepth.size(); ++frame) {
        auto attachment = m_staticScene->createAttachment(
            shadowExtent,
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!attachment) {
            return std::unexpected(std::move(attachment.error()));
        }
        m_shadowDepth[frame] = *attachment;
    }
    if (m_heapBinding) {
        if (auto written = writeShadowHeapDescriptors(); !written) {
            resetShadowHeapDescriptors();
            return written;
        }
    }

    const bool indirect = m_staticScene && m_staticScene->indirectEnabled();
    if (!m_heapBinding) {
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0u, 64u};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.pushConstantRangeCount = 1u;
        layoutInfo.pPushConstantRanges = &push;
        if (indirect) {
            const VkDescriptorSetLayout indirectSetLayout = m_staticScene->indirectLayout();
            layoutInfo.setLayoutCount = 1u;
            layoutInfo.pSetLayouts = &indirectSetLayout;
            if (auto created = create(
                    m_device,
                    m_indirectShadowLayout,
                    layoutInfo,
                    vkCreatePipelineLayout,
                    "indirect shadow pipeline layout");
                !created) {
                return created;
            }
        } else {
            if (auto created = create(
                    m_device,
                    m_directShadowLayout,
                    layoutInfo,
                    vkCreatePipelineLayout,
                    "direct shadow pipeline layout");
                !created) {
                return created;
            }
        }
    }
    if (indirect) {
        return createShadowPipeline(
            true, shadow.indirectVertex, m_indirectShadowLayout, m_indirectShadowPipeline);
    }
    return createShadowPipeline(
        false, shadow.directVertex, m_directShadowLayout, m_directShadowPipeline);
}

std::expected<void, core::Error>
Presentation::writeLightingFrame(std::size_t frameIndex, bool activeShadows) {
    LightingFrameGpu gpu{};
    gpu.inverseProjection = m_frameView.inverseProjection;
    gpu.inverseProjection[0] *= static_cast<float>(m_swapchain.extent.width) /
                                static_cast<float>(m_swapchain.extent.height);
    gpu.viewToWorld = m_frameView.viewToWorld;
    gpu.lightViewProj = m_lightingState.lightViewProj;

    if (m_lightingState.legacyViewSpace) {
        gpu.lightDirView = {0.F, 0.F, 1.F, m_lightingState.intensity};
    } else {
        const float lx = -m_lightingState.directionWorld[0];
        const float ly = -m_lightingState.directionWorld[1];
        const float lz = -m_lightingState.directionWorld[2];
        const auto& view = m_frameView.view;
        float vx = view[0] * lx + view[4] * ly + view[8] * lz;
        float vy = view[1] * lx + view[5] * ly + view[9] * lz;
        float vz = view[2] * lx + view[6] * ly + view[10] * lz;
        const float length = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (length > 1e-6F) {
            vx /= length;
            vy /= length;
            vz /= length;
        } else {
            vx = 0.F;
            vy = 0.F;
            vz = 1.F;
        }
        gpu.lightDirView = {vx, vy, vz, m_lightingState.intensity};
    }

    gpu.lightColor = {
        m_lightingState.color[0],
        m_lightingState.color[1],
        m_lightingState.color[2],
        activeShadows ? 1.0F : 0.0F};
    const float invResolution = (m_lighting && m_lighting->shadow)
                                    ? (1.0F / static_cast<float>(m_lighting->shadow->resolution))
                                    : 0.0F;
    gpu.shadowParams = {
        m_lightingState.normalBias,
        m_lightingState.depthBias,
        invResolution,
        m_lightingState.pcfRadiusTexels};

    return m_staticScene->writeUniformBuffer(
        m_lightingFrames[frameIndex], std::as_bytes(std::span{&gpu, 1}));
}

void Presentation::recordShadow(
    VkCommandBuffer commands,
    VkQueryPool queryPool,
    const Image& output,
    const PresentationGraphPlan& plan) const {
    writeTimestamp(
        commands, queryPool, TimestampSlot::ShadowBegin, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    recordGraphBarriers(commands, output, plan, PresentationPass::Shadow);

    const std::uint32_t resolution = m_lighting->shadow->resolution;
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = m_shadowDepth[m_frameIndex].view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {1.0F, 0};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {resolution, resolution}};
    rendering.layerCount = 1u;
    rendering.colorAttachmentCount = 0u;
    rendering.pColorAttachments = nullptr;
    rendering.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(commands, &rendering);
    const bool indirect = m_staticScene && m_staticScene->indirectEnabled();
    const VkPipeline pipeline = indirect ? m_indirectShadowPipeline : m_directShadowPipeline;
    const VkPipelineLayout layout = indirect ? m_indirectShadowLayout : m_directShadowLayout;
    if (pipeline && m_staticScene) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        m_staticScene->recordShadow(commands, layout, m_lightingState.lightViewProj, m_frameIndex);
    }
    vkCmdEndRendering(commands);
    writeTimestamp(
        commands, queryPool, TimestampSlot::ShadowEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
}

std::expected<void, core::Error> Presentation::writeAttachmentHeapDescriptors() {
    if (!m_heapBinding) {
        return {};
    }
    const auto make2DView = [](VkImage image, VkFormat format, VkImageAspectFlags aspect) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY};
        view.subresourceRange = {aspect, 0, 1, 0, 1};
        return view;
    };
    const VkFormat depthFormat = toVkFormat(m_spec.depthFormat);
    const VkFormat hdrFormat = toVkFormat(m_spec.hdrFormat);
    for (std::size_t frame = 0; frame < m_swapchain.attachments.size(); ++frame) {
        const auto& slot = m_swapchain.attachments[frame];
        const auto base = static_cast<std::uint32_t>(
            kHeapExternalImageBase + frame * kHeapAttachmentSlotsPerFrame);
        const std::array<VkImageViewCreateInfo, kHeapAttachmentSlotsPerFrame> views{
            make2DView(
                slot.gbuffer[0].image,
                toVkFormat(m_spec.gbufferFormats[0]),
                VK_IMAGE_ASPECT_COLOR_BIT),
            make2DView(
                slot.gbuffer[1].image,
                toVkFormat(m_spec.gbufferFormats[1]),
                VK_IMAGE_ASPECT_COLOR_BIT),
            make2DView(
                slot.gbuffer[2].image,
                toVkFormat(m_spec.gbufferFormats[2]),
                VK_IMAGE_ASPECT_COLOR_BIT),
            make2DView(slot.depth.image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT),
            make2DView(slot.hdr.image, hdrFormat, VK_IMAGE_ASPECT_COLOR_BIT),
        };
        for (std::uint32_t i = 0; i < views.size(); ++i) {
            if (auto written = m_heapBinding->writeExternalImageSlot(base + i, views[i]);
                !written) {
                return written;
            }
        }
    }
    return {};
}

void Presentation::resetAttachmentHeapDescriptors() noexcept {
    if (!m_heapBinding) {
        return;
    }
    constexpr std::uint32_t count = kHeapFrameSlotCount * kHeapAttachmentSlotsPerFrame;
    for (std::uint32_t i = 0; i < count; ++i) {
        static_cast<void>(
            m_heapBinding->resetExternalImageSlotToDefault(kHeapExternalImageBase + i));
    }
}

std::expected<void, core::Error>
Presentation::writeEnvironmentHeapDescriptors(const StaticSceneDraw::Environment& environment) {
    if (!m_heapBinding) {
        return {};
    }
    const auto makeEnvView = [](VkImage image,
                                VkImageViewType viewType,
                                VkFormat format,
                                std::uint32_t levels,
                                std::uint32_t layers) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image;
        view.viewType = viewType;
        view.format = format;
        view.components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY};
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        return view;
    };
    const std::array<VkImageViewCreateInfo, kHeapEnvironmentImageCount> views{
        makeEnvView(
            environment[0].image, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R16G16B16A16_SFLOAT, 1u, 6u),
        makeEnvView(
            environment[1].image, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R16G16B16A16_SFLOAT, 5u, 6u),
        makeEnvView(environment[2].image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R16G16_SFLOAT, 1u, 1u),
    };
    for (std::uint32_t i = 0; i < views.size(); ++i) {
        if (auto written =
                m_heapBinding->writeExternalImageSlot(kHeapEnvironmentImageBase + i, views[i]);
            !written) {
            return written;
        }
    }
    return {};
}

void Presentation::resetEnvironmentHeapDescriptors() noexcept {
    if (!m_heapBinding) {
        return;
    }
    for (std::uint32_t i = 0; i < kHeapEnvironmentImageCount; ++i) {
        static_cast<void>(
            m_heapBinding->resetExternalImageSlotToDefault(kHeapEnvironmentImageBase + i));
    }
}

std::expected<void, core::Error> Presentation::createLightingResources(VkImageUsageFlags usage) {
    const VkFormat hdrFormat = toVkFormat(m_spec.hdrFormat);
    if (m_heapBinding) {
        for (auto& slot : m_swapchain.attachments) {
            auto attachment = m_staticScene->createAttachment(
                m_swapchain.extent, hdrFormat, VK_IMAGE_ASPECT_COLOR_BIT, usage);
            if (!attachment) {
                return std::unexpected(attachment.error());
            }
            slot.hdr = *attachment;
        }
        if (auto written = writeAttachmentHeapDescriptors(); !written) {
            resetAttachmentHeapDescriptors();
            return written;
        }
        if (m_environmentReady) {
            if (auto written = writeEnvironmentHeapDescriptors(m_environment); !written) {
                return written;
            }
        }
        return {};
    }
    constexpr std::uint32_t setsPerSlot = 2;
    constexpr std::uint32_t gbufferBindings = 4;
    const std::uint32_t combinedPerDirect = gbufferBindings +
                                            (m_lighting->environmentLighting ? 3u : 0u) +
                                            (m_lighting->shadow ? 1u : 0u);
    constexpr std::uint32_t displayBindings = 6u;
    const auto slotCount = static_cast<std::uint32_t>(m_swapchain.attachments.size());
    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         slotCount * (combinedPerDirect + displayBindings)},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, slotCount},
    }};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = setsPerSlot * slotCount;
    pool.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    pool.pPoolSizes = poolSizes.data();
    if (auto created = create(
            m_device,
            m_swapchain.lightingPool,
            pool,
            vkCreateDescriptorPool,
            "lighting descriptor pool");
        !created) {
        return created;
    }

    for (std::size_t frame = 0; frame < m_swapchain.attachments.size(); ++frame) {
        auto& slot = m_swapchain.attachments[frame];
        auto attachment = m_staticScene->createAttachment(
            m_swapchain.extent, hdrFormat, VK_IMAGE_ASPECT_COLOR_BIT, usage);
        if (!attachment) {
            return std::unexpected(attachment.error());
        }
        slot.hdr = *attachment;
        const std::array layouts{m_lightingSetLayout, m_displaySetLayout};
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
        const std::array directViews{
            slot.gbuffer[0].view, slot.gbuffer[1].view, slot.gbuffer[2].view, slot.depth.view};
        const std::array displayViews{
            slot.hdr.view,
            slot.gbuffer[0].view,
            slot.gbuffer[1].view,
            slot.gbuffer[2].view,
            slot.depth.view,
            m_lighting->shadow ? m_shadowDepth[frame].view : slot.depth.view};
        std::array<VkDescriptorImageInfo, 11> images{};
        std::array<VkWriteDescriptorSet, 12> writes{};
        std::uint32_t imageCount = 0;
        std::uint32_t writeCount = 0;
        for (std::uint32_t i = 0; i < directViews.size(); ++i) {
            images[imageCount] = {
                VK_NULL_HANDLE, directViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            auto& write = writes[writeCount++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = slot.directSet;
            write.dstBinding = i;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &images[imageCount++];
        }
        for (std::uint32_t i = 0; i < displayViews.size(); ++i) {
            images[imageCount] = {
                VK_NULL_HANDLE, displayViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            auto& write = writes[writeCount++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = slot.displaySet;
            write.dstBinding = i;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &images[imageCount++];
        }
        if (m_lighting->shadow) {
            images[imageCount] = {
                VK_NULL_HANDLE,
                m_shadowDepth[frame].view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            auto& write = writes[writeCount++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = slot.directSet;
            write.dstBinding = 7u;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &images[imageCount++];
        }
        const VkDescriptorBufferInfo uboInfo{
            m_lightingFrames[frame].buffer, 0, VkDeviceSize{sizeof(LightingFrameGpu)}};
        auto& uboWrite = writes[writeCount++];
        uboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        uboWrite.dstSet = slot.directSet;
        uboWrite.dstBinding = 8u;
        uboWrite.descriptorCount = 1;
        uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboWrite.pBufferInfo = &uboInfo;
        vkUpdateDescriptorSets(m_device, writeCount, writes.data(), 0, nullptr);
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
    VkCommandBuffer commands,
    VkQueryPool queryPool,
    const Image& output,
    const PresentationGraphPlan& plan,
    DebugViewMode effectiveView) const {
    const auto& slot = m_swapchain.attachments[m_frameIndex];
    writeTimestamp(
        commands, queryPool, TimestampSlot::LightingBegin, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    recordGraphBarriers(commands, output, plan, PresentationPass::Lighting);

    const auto draw = [&](VkImageView target,
                          VkPipeline pipeline,
                          VkPipelineLayout layout,
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
        if (m_heapBinding) {
            VkPushDataInfoEXT pushInfo{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
            pushInfo.offset = 0;
            pushInfo.data = {push, size};
            m_heapBinding->dispatch().cmdPushData(commands, &pushInfo);
        } else {
            vkCmdBindDescriptorSets(
                commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
            if (size > 0u) {
                vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, push);
            }
        }
        vkCmdDraw(commands, 3, 1, 0, 0);
        vkCmdEndRendering(commands);
    };

    if (m_heapBinding) {
        const std::uint32_t frameSlot = static_cast<std::uint32_t>(m_frameIndex);
        draw(
            slot.hdr.view,
            m_swapchain.directPipeline,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            sizeof(frameSlot),
            &frameSlot);
    } else {
        draw(
            slot.hdr.view,
            m_swapchain.directPipeline,
            m_lightingLayout,
            slot.directSet,
            0u,
            nullptr);
    }
    writeTimestamp(
        commands, queryPool, TimestampSlot::LightingEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

    writeTimestamp(
        commands, queryPool, TimestampSlot::DisplayBegin, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    recordGraphBarriers(commands, output, plan, PresentationPass::Display);

    const bool srgb = m_swapchain.format == VK_FORMAT_B8G8R8A8_SRGB ||
                      m_swapchain.format == VK_FORMAT_R8G8B8A8_SRGB;
    const DisplayPushData display = m_spec.buildDisplayPush(
        DisplayPushParams{
            m_spec.displayExposureEV,
            srgb,
            effectiveView,
            m_frameView.nearPlane,
            m_frameView.farPlane});
    if (m_heapBinding) {
        struct DisplayHeapPush {
            std::array<std::byte, 20> payload{};
            std::uint32_t frameSlot = 0;
        };
        static_assert(sizeof(DisplayHeapPush) == 24u);
        DisplayHeapPush displayPush{};
        std::memcpy(displayPush.payload.data(), display.bytes.data(), display.size);
        displayPush.frameSlot = static_cast<std::uint32_t>(m_frameIndex);
        draw(
            output.view,
            m_swapchain.displayPipeline,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            sizeof(displayPush),
            &displayPush);
    } else {
        draw(
            output.view,
            m_swapchain.displayPipeline,
            m_displayLayout,
            slot.displaySet,
            display.size,
            display.bytes.data());
    }
    writeTimestamp(
        commands, queryPool, TimestampSlot::DisplayEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
}

std::expected<void, core::Error> Presentation::createGraphicsPipeline(
    VkFormat format,
    const std::vector<std::uint32_t>& vertexCode,
    const std::vector<std::uint32_t>& fragmentCode,
    VkPipeline& output,
    PipelineKind kind) {
    const bool fullscreen = kind == PipelineKind::DeferredLighting || kind == PipelineKind::Display;
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
    std::array<VkDescriptorSetAndBindingMappingEXT, 2> vertexMappings{};
    std::uint32_t vertexMappingCount = 0;
    VkShaderDescriptorSetAndBindingMappingInfoEXT vertexMappingInfo{
        VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT};
    std::array<VkDescriptorSetAndBindingMappingEXT, 9> fragmentMappings{};
    std::uint32_t fragmentMappingCount = 0;
    VkShaderDescriptorSetAndBindingMappingInfoEXT fragmentMappingInfo{
        VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT};
    if (m_heapBinding) {
        const auto& plan = m_heapBinding->plan();
        const auto imageStride = static_cast<std::uint32_t>(plan.imageStride);
        const auto samplerStride = static_cast<std::uint32_t>(plan.samplerStride);
        const auto bufferBase = static_cast<std::uint32_t>(plan.bufferBaseOffset);
        const auto bufferStride = static_cast<std::uint32_t>(plan.bufferStride);
        const auto frameAttachmentStride = kHeapAttachmentSlotsPerFrame * imageStride;

        const auto addConstant = [](VkDescriptorSetAndBindingMappingEXT& dst,
                                    std::uint32_t set,
                                    std::uint32_t binding,
                                    VkSpirvResourceTypeFlagsEXT mask,
                                    std::uint32_t heapOffset,
                                    std::uint32_t arrayStride,
                                    std::uint32_t samplerHeapOffset = 0u) {
            dst = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT};
            dst.descriptorSet = set;
            dst.firstBinding = binding;
            dst.bindingCount = 1;
            dst.resourceMask = mask;
            dst.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
            dst.sourceData.constantOffset.heapOffset = heapOffset;
            dst.sourceData.constantOffset.heapArrayStride = arrayStride;
            dst.sourceData.constantOffset.pEmbeddedSampler = nullptr;
            dst.sourceData.constantOffset.samplerHeapOffset = samplerHeapOffset;
            dst.sourceData.constantOffset.samplerHeapArrayStride = 0;
        };

        const auto addPushIndex = [](VkDescriptorSetAndBindingMappingEXT& dst,
                                     std::uint32_t set,
                                     std::uint32_t binding,
                                     VkSpirvResourceTypeFlagsEXT mask,
                                     std::uint32_t heapOffset,
                                     std::uint32_t pushOffset,
                                     std::uint32_t indexStride,
                                     std::uint32_t samplerHeapOffset = 0u) {
            dst = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT};
            dst.descriptorSet = set;
            dst.firstBinding = binding;
            dst.bindingCount = 1;
            dst.resourceMask = mask;
            dst.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT;
            dst.sourceData.pushIndex.heapOffset = heapOffset;
            dst.sourceData.pushIndex.pushOffset = pushOffset;
            dst.sourceData.pushIndex.heapIndexStride = indexStride;
            dst.sourceData.pushIndex.heapArrayStride = 0;
            dst.sourceData.pushIndex.pEmbeddedSampler = nullptr;
            dst.sourceData.pushIndex.useCombinedImageSamplerIndex = VK_FALSE;
            dst.sourceData.pushIndex.samplerHeapOffset = samplerHeapOffset;
            dst.sourceData.pushIndex.samplerPushOffset = pushOffset;
            dst.sourceData.pushIndex.samplerHeapIndexStride = 0;
            dst.sourceData.pushIndex.samplerHeapArrayStride = 0;
        };

        switch (kind) {
        case PipelineKind::DirectGeometry:
            addConstant(
                fragmentMappings[fragmentMappingCount++],
                0u,
                0u,
                VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT,
                0u,
                imageStride);
            addConstant(
                fragmentMappings[fragmentMappingCount++],
                0u,
                1u,
                VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT,
                0u,
                0u);
            break;
        case PipelineKind::IndirectGeometry:
            addConstant(
                vertexMappings[vertexMappingCount++],
                1u,
                0u,
                VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT,
                bufferBase,
                0u);
            addPushIndex(
                vertexMappings[vertexMappingCount++],
                1u,
                4u,
                VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT,
                bufferBase + 6u * bufferStride,
                0u,
                bufferStride);
            addConstant(
                fragmentMappings[fragmentMappingCount++],
                0u,
                0u,
                VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT,
                0u,
                imageStride);
            addConstant(
                fragmentMappings[fragmentMappingCount++],
                0u,
                1u,
                VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT,
                0u,
                0u);
            addConstant(
                fragmentMappings[fragmentMappingCount++],
                1u,
                1u,
                VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT,
                bufferBase + 1u * bufferStride,
                0u);
            break;
        case PipelineKind::DeferredLighting:
            for (std::uint32_t b = 0; b < 4u; ++b) {
                addPushIndex(
                    fragmentMappings[fragmentMappingCount++],
                    0u,
                    b,
                    VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT,
                    (kHeapExternalImageBase + b) * imageStride,
                    0u,
                    frameAttachmentStride,
                    1u * samplerStride);
            }
            if (m_lighting && m_lighting->environmentLighting) {
                for (std::uint32_t k = 0; k < kHeapEnvironmentImageCount; ++k) {
                    addConstant(
                        fragmentMappings[fragmentMappingCount++],
                        0u,
                        4u + k,
                        VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT,
                        (kHeapEnvironmentImageBase + k) * imageStride,
                        0u,
                        2u * samplerStride);
                }
            }
            if (m_lighting && m_lighting->shadow) {
                addPushIndex(
                    fragmentMappings[fragmentMappingCount++],
                    0u,
                    7u,
                    VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT,
                    kHeapShadowImageBase * imageStride,
                    0u,
                    imageStride,
                    3u * samplerStride);
            }
            addPushIndex(
                fragmentMappings[fragmentMappingCount++],
                0u,
                8u,
                VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT,
                bufferBase + kHeapLightingBufferBase * bufferStride,
                0u,
                bufferStride);
            break;
        case PipelineKind::Display:
            // Binding 0=HDR, 1=BaseColor, 2=Normal, 3=Material, 4=MainDepth, 5=ShadowDepth (or
            // MainDepth fallback when shadow pipeline is off), all using non-compare sampler 1.
            addPushIndex(
                fragmentMappings[fragmentMappingCount++],
                0u,
                0u,
                VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT,
                (kHeapExternalImageBase + 4u) * imageStride,
                20u,
                frameAttachmentStride,
                1u * samplerStride);
            for (std::uint32_t b = 0u; b < 4u; ++b) {
                addPushIndex(
                    fragmentMappings[fragmentMappingCount++],
                    0u,
                    1u + b,
                    VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT,
                    (kHeapExternalImageBase + b) * imageStride,
                    20u,
                    frameAttachmentStride,
                    1u * samplerStride);
            }
            if (m_lighting && m_lighting->shadow) {
                addPushIndex(
                    fragmentMappings[fragmentMappingCount++],
                    0u,
                    5u,
                    VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT,
                    kHeapShadowImageBase * imageStride,
                    20u,
                    imageStride,
                    1u * samplerStride);
            } else {
                addPushIndex(
                    fragmentMappings[fragmentMappingCount++],
                    0u,
                    5u,
                    VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT,
                    (kHeapExternalImageBase + 3u) * imageStride,
                    20u,
                    frameAttachmentStride,
                    1u * samplerStride);
            }
            break;
        }

        if (vertexMappingCount > 0) {
            vertexMappingInfo.mappingCount = vertexMappingCount;
            vertexMappingInfo.pMappings = vertexMappings.data();
            stageInfos[0].pNext = &vertexMappingInfo;
        }
        if (fragmentMappingCount > 0) {
            fragmentMappingInfo.mappingCount = fragmentMappingCount;
            fragmentMappingInfo.pMappings = fragmentMappings.data();
            stageInfos[1].pNext = &fragmentMappingInfo;
        }

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
        m_heapBinding ? VK_NULL_HANDLE
        : kind == PipelineKind::Display
            ? m_displayLayout
            : (kind == PipelineKind::DeferredLighting ? m_lightingLayout : m_pipelineLayout);
    const VkResult result =
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &output);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateGraphicsPipelines", result));
    }
    return {};
}

} // namespace ar::rhi::detail
