#include "OverlayDraw.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include <vk_mem_alloc.h>

#include "PresentationUtil.h"
#include "binding/DescriptorHeapLayout.h"
#include "binding/DescriptorHeapTable.h"

namespace ar::rhi::detail {
namespace {

using presentation::create;
using presentation::failed;

struct alignas(4) OverlayPushGpu {
    float scaleX = 0.F;
    float scaleY = 0.F;
    float translateX = 0.F;
    float translateY = 0.F;
    std::uint32_t decodeSrgb = 0u;
};

static_assert(sizeof(OverlayPushGpu) == 20u);

constexpr VkImageViewCreateInfo makeFontViewInfo(VkImage image) noexcept {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY};
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    return view;
}

} // namespace

OverlayDraw::OverlayDraw(
    VkDevice device,
    DeviceResources& resources,
    DescriptorHeapTable* heapBinding,
    const OverlayShaders& shaders) noexcept
    : m_device(device), m_resources(resources), m_heapBinding(heapBinding), m_shaders(shaders) {}

OverlayDraw::~OverlayDraw() {
    close();
}

std::expected<void, core::Error> OverlayDraw::open() {
    if (m_heapBinding) {
        // Heap mode reuses the existing Resource Heap external slot kHeapOverlayFontImage and
        // Sampler Heap slot 2 (linear clamp), creating zero traditional descriptor objects.
        return {};
    }

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (auto created = create(m_device, m_sampler, sampler, vkCreateSampler, "overlay sampler");
        !created) {
        return created;
    }

    const VkDescriptorSetLayoutBinding binding{
        0u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        1u,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        &m_sampler};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 1u;
    setInfo.pBindings = &binding;
    if (auto created = create(
            m_device,
            m_setLayout,
            setInfo,
            vkCreateDescriptorSetLayout,
            "overlay descriptor set layout");
        !created) {
        return created;
    }

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 1u;
    poolInfo.pPoolSizes = &poolSize;
    if (auto created =
            create(m_device, m_pool, poolInfo, vkCreateDescriptorPool, "overlay descriptor pool");
        !created) {
        return created;
    }

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = m_pool;
    allocInfo.descriptorSetCount = 1u;
    allocInfo.pSetLayouts = &m_setLayout;
    if (const VkResult result = vkAllocateDescriptorSets(m_device, &allocInfo, &m_set);
        result != VK_SUCCESS) {
        return std::unexpected(failed("allocating overlay descriptor set", result));
    }

    const VkPushConstantRange pushRange{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0u,
        static_cast<std::uint32_t>(sizeof(OverlayPushGpu))};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1u;
    layoutInfo.pSetLayouts = &m_setLayout;
    layoutInfo.pushConstantRangeCount = 1u;
    layoutInfo.pPushConstantRanges = &pushRange;
    return create(
        m_device, m_pipelineLayout, layoutInfo, vkCreatePipelineLayout, "overlay pipeline layout");
}

std::expected<void, core::Error> OverlayDraw::createPipeline(VkFormat colorFormat) {
    destroyPipeline();

    struct ShaderModules {
        VkDevice device;
        std::array<VkShaderModule, 2> handles{};
        ~ShaderModules() {
            for (const auto handle : handles) {
                if (handle) {
                    vkDestroyShaderModule(device, handle, nullptr);
                }
            }
        }
    } modules{m_device};

    const std::array bytecode{&m_shaders.vertex, &m_shaders.fragment};
    const std::array stages{VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    std::array<VkPipelineShaderStageCreateInfo, 2> stageInfos{};
    for (std::size_t i = 0; i < stageInfos.size(); ++i) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = bytecode[i]->size() * sizeof(std::uint32_t);
        info.pCode = bytecode[i]->data();
        if (auto created = create(
                m_device, modules.handles[i], info, vkCreateShaderModule, "overlay shader module");
            !created) {
            return created;
        }
        stageInfos[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfos[i].stage = stages[i];
        stageInfos[i].module = modules.handles[i];
        stageInfos[i].pName = "main";
    }

    const VkVertexInputBindingDescription vertexBinding{
        0u, sizeof(OverlayVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array vertexAttributes{
        VkVertexInputAttributeDescription{
            0u, 0u, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, x)},
        VkVertexInputAttributeDescription{
            1u, 0u, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, u)},
        VkVertexInputAttributeDescription{
            2u, 0u, VK_FORMAT_R8G8B8A8_UNORM, offsetof(OverlayVertex, rgba8)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1u;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<std::uint32_t>(vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions = vertexAttributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1u;
    viewportState.scissorCount = 1u;

    constexpr std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1u;
    blend.pAttachments = &blendAttachment;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1u;
    rendering.pColorAttachmentFormats = &colorFormat;

    VkDescriptorSetAndBindingMappingEXT fontMapping{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT};
    VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{
        VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT};
    VkPipelineCreateFlags2CreateInfo flags2{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};

    if (m_heapBinding) {
        const auto& plan = m_heapBinding->plan();
        fontMapping.descriptorSet = 0u;
        fontMapping.firstBinding = 0u;
        fontMapping.bindingCount = 1u;
        fontMapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT;
        fontMapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
        fontMapping.sourceData.constantOffset.heapOffset =
            static_cast<std::uint32_t>(kHeapOverlayFontImage * plan.imageStride);
        fontMapping.sourceData.constantOffset.heapArrayStride = 0u;
        fontMapping.sourceData.constantOffset.samplerHeapOffset =
            static_cast<std::uint32_t>(2u * plan.samplerStride);
        fontMapping.sourceData.constantOffset.samplerHeapArrayStride = 0u;

        mappingInfo.mappingCount = 1u;
        mappingInfo.pMappings = &fontMapping;
        stageInfos[1].pNext = &mappingInfo;

        flags2.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
        flags2.pNext = &rendering;
    }

    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.pNext =
        m_heapBinding ? static_cast<const void*>(&flags2) : static_cast<const void*>(&rendering);
    pipeline.stageCount = static_cast<std::uint32_t>(stageInfos.size());
    pipeline.pStages = stageInfos.data();
    pipeline.pVertexInputState = &vertexInput;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewportState;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamicState;
    pipeline.layout = m_heapBinding ? VK_NULL_HANDLE : m_pipelineLayout;

    const VkResult result =
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1u, &pipeline, nullptr, &m_pipeline);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateGraphicsPipelines(overlay)", result));
    }
    return {};
}

void OverlayDraw::destroyPipeline() noexcept {
    if (m_pipeline) {
        vkDestroyPipeline(m_device, std::exchange(m_pipeline, VK_NULL_HANDLE), nullptr);
    }
}

std::expected<void, core::Error> OverlayDraw::uploadFont(const OverlayFontAtlas& atlas) {
    if (atlas.width == 0u || atlas.height == 0u ||
        static_cast<std::uint64_t>(atlas.width) * atlas.height * 4u != atlas.rgba.size()) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Validation,
                "overlay font atlas requires positive dimensions and RGBA8 pixel bytes",
                std::nullopt});
    }

    auto imageHandle = m_resources.createSampledImage(
        {atlas.width, atlas.height}, std::as_bytes(std::span{atlas.rgba}));
    if (!imageHandle) {
        return std::unexpected(std::move(imageHandle.error()));
    }

    if (m_heapBinding) {
        auto vkImage = m_resources.retainSampledImage(*imageHandle);
        if (!vkImage) {
            static_cast<void>(m_resources.destroyImage(*imageHandle));
            return std::unexpected(std::move(vkImage.error()));
        }
        const VkImageViewCreateInfo viewInfo = makeFontViewInfo(*vkImage);
        if (auto written = m_heapBinding->writeExternalImageSlot(kHeapOverlayFontImage, viewInfo);
            !written) {
            m_resources.releaseSampledImage(*imageHandle);
            static_cast<void>(m_resources.destroyImage(*imageHandle));
            return written;
        }
        // Descriptor slot now references the new image; release only the previous image resources.
        if (m_fontImageHandle.generation != 0u) {
            if (m_fontImage) {
                m_resources.releaseSampledImage(m_fontImageHandle);
                m_fontImage = VK_NULL_HANDLE;
            }
            static_cast<void>(m_resources.destroyImage(m_fontImageHandle));
            m_fontImageHandle = {};
        }
        m_fontImageHandle = *imageHandle;
        m_fontImage = *vkImage;
        m_fontReady = true;
        return {};
    }

    auto viewHandle = m_resources.createImageView(*imageHandle);
    if (!viewHandle) {
        static_cast<void>(m_resources.destroyImage(*imageHandle));
        return std::unexpected(std::move(viewHandle.error()));
    }
    auto vkView = m_resources.retainSampledView(*viewHandle);
    if (!vkView) {
        static_cast<void>(m_resources.destroyImageView(*viewHandle));
        static_cast<void>(m_resources.destroyImage(*imageHandle));
        return std::unexpected(std::move(vkView.error()));
    }

    const VkDescriptorImageInfo imageInfo{
        VK_NULL_HANDLE, *vkView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_set;
    write.dstBinding = 0u;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_device, 1u, &write, 0u, nullptr);

    releaseFont();
    m_fontImageHandle = *imageHandle;
    m_fontViewHandle = *viewHandle;
    m_fontView = *vkView;
    m_fontReady = true;
    return {};
}

std::expected<void, core::Error> OverlayDraw::ensureBuffer(
    DeviceResources::BufferSlot& slot, std::size_t requiredBytes, VkBufferUsageFlags usage) {
    if (slot.buffer && slot.desc.bytes >= requiredBytes) {
        return {};
    }
    const std::size_t nextBytes =
        std::max<std::size_t>(requiredBytes, std::max<std::size_t>(slot.desc.bytes * 2u, 4096u));
    auto created = m_resources.allocateBuffer({nextBytes, BufferMemory::HostVisible}, usage);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }
    if (slot.buffer && m_resources.allocator()) {
        vmaDestroyBuffer(m_resources.allocator(), slot.buffer, slot.allocation);
    }
    slot = *created;
    return {};
}

std::expected<bool, core::Error>
OverlayDraw::prepareFrame(std::size_t frameIndex, const OverlayDrawList& list) {
    if (!m_fontReady || !m_pipeline || list.vertices.empty() || list.indices.empty() ||
        list.commands.empty() || !std::isfinite(list.displayWidth) ||
        !std::isfinite(list.displayHeight) || !(list.displayWidth > 0.F) ||
        !(list.displayHeight > 0.F)) {
        return false;
    }

    auto& frame = m_frames[frameIndex];
    const auto vertexBytes = std::as_bytes(std::span{list.vertices});
    const auto indexBytes = std::as_bytes(std::span{list.indices});
    if (auto ok =
            ensureBuffer(frame.vertices, vertexBytes.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        !ok) {
        return std::unexpected(std::move(ok.error()));
    }
    if (auto ok = ensureBuffer(frame.indices, indexBytes.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        !ok) {
        return std::unexpected(std::move(ok.error()));
    }
    if (auto written = m_resources.writeAllocation(frame.vertices.allocation, 0u, vertexBytes);
        !written) {
        return std::unexpected(std::move(written.error()));
    }
    if (auto written = m_resources.writeAllocation(frame.indices.allocation, 0u, indexBytes);
        !written) {
        return std::unexpected(std::move(written.error()));
    }
    return true;
}

void OverlayDraw::record(
    VkCommandBuffer commands,
    VkImageView targetView,
    VkExtent2D extent,
    bool swapchainIsSrgb,
    std::size_t frameIndex,
    const OverlayDrawList& list) const {
    if (!m_pipeline || !m_fontReady || extent.width == 0u || extent.height == 0u) {
        return;
    }
    const auto& frame = m_frames[frameIndex];
    if (!frame.vertices.buffer || !frame.indices.buffer) {
        return;
    }

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = targetView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1u;
    rendering.colorAttachmentCount = 1u;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(commands, &rendering);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    const VkViewport viewport{
        0.F, 0.F, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.F, 1.F};
    vkCmdSetViewport(commands, 0u, 1u, &viewport);

    const OverlayPushGpu push{
        2.0F / list.displayWidth,
        2.0F / list.displayHeight,
        -1.0F,
        -1.0F,
        swapchainIsSrgb ? 1u : 0u};
    if (m_heapBinding) {
        VkPushDataInfoEXT pushInfo{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
        pushInfo.offset = 0u;
        pushInfo.data = {&push, sizeof(push)};
        m_heapBinding->dispatch().cmdPushData(commands, &pushInfo);
    } else {
        vkCmdBindDescriptorSets(
            commands,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_pipelineLayout,
            0u,
            1u,
            &m_set,
            0u,
            nullptr);
        vkCmdPushConstants(
            commands,
            m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0u,
            sizeof(push),
            &push);
    }

    const VkDeviceSize offset = 0u;
    vkCmdBindVertexBuffers(commands, 0u, 1u, &frame.vertices.buffer, &offset);
    vkCmdBindIndexBuffer(commands, frame.indices.buffer, 0u, VK_INDEX_TYPE_UINT16);

    const std::size_t indexLimit = list.indices.size();
    const std::int32_t maxW = static_cast<std::int32_t>(extent.width);
    const std::int32_t maxH = static_cast<std::int32_t>(extent.height);
    for (const auto& cmd : list.commands) {
        if (cmd.indexCount == 0u || cmd.indexOffset > indexLimit ||
            cmd.indexCount > indexLimit - cmd.indexOffset) {
            continue;
        }
        const std::int32_t x0 = std::clamp(cmd.clipX, 0, maxW);
        const std::int32_t y0 = std::clamp(cmd.clipY, 0, maxH);
        const std::int64_t rawX1 =
            static_cast<std::int64_t>(cmd.clipX) + static_cast<std::int64_t>(cmd.clipWidth);
        const std::int64_t rawY1 =
            static_cast<std::int64_t>(cmd.clipY) + static_cast<std::int64_t>(cmd.clipHeight);
        const std::int32_t x1 = static_cast<std::int32_t>(
            std::clamp<std::int64_t>(rawX1, static_cast<std::int64_t>(x0), maxW));
        const std::int32_t y1 = static_cast<std::int32_t>(
            std::clamp<std::int64_t>(rawY1, static_cast<std::int64_t>(y0), maxH));
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        const VkRect2D scissor{
            {x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
        vkCmdSetScissor(commands, 0u, 1u, &scissor);
        vkCmdDrawIndexed(commands, cmd.indexCount, 1u, cmd.indexOffset, cmd.vertexOffset, 0u);
    }

    vkCmdEndRendering(commands);
}

void OverlayDraw::releaseFont() noexcept {
    if (m_heapBinding && m_fontImage) {
        static_cast<void>(m_heapBinding->resetExternalImageSlotToDefault(kHeapOverlayFontImage));
        if (m_fontImageHandle.generation != 0u) {
            m_resources.releaseSampledImage(m_fontImageHandle);
        }
        m_fontImage = VK_NULL_HANDLE;
    }
    if (m_fontView) {
        m_resources.releaseSampledView(m_fontView);
        m_fontView = VK_NULL_HANDLE;
    }
    if (m_fontViewHandle.generation != 0u) {
        static_cast<void>(m_resources.destroyImageView(m_fontViewHandle));
        m_fontViewHandle = {};
    }
    if (m_fontImageHandle.generation != 0u) {
        static_cast<void>(m_resources.destroyImage(m_fontImageHandle));
        m_fontImageHandle = {};
    }
    m_fontReady = false;
}

void OverlayDraw::close() noexcept {
    destroyPipeline();
    releaseFont();
    if (m_resources.allocator()) {
        for (auto& frame : m_frames) {
            if (frame.vertices.buffer) {
                vmaDestroyBuffer(
                    m_resources.allocator(), frame.vertices.buffer, frame.vertices.allocation);
            }
            if (frame.indices.buffer) {
                vmaDestroyBuffer(
                    m_resources.allocator(), frame.indices.buffer, frame.indices.allocation);
            }
            frame = {};
        }
    } else {
        m_frames = {};
    }
    if (m_device) {
        if (m_pipelineLayout) {
            vkDestroyPipelineLayout(
                m_device, std::exchange(m_pipelineLayout, VK_NULL_HANDLE), nullptr);
        }
        if (m_pool) {
            vkDestroyDescriptorPool(m_device, std::exchange(m_pool, VK_NULL_HANDLE), nullptr);
            m_set = VK_NULL_HANDLE;
        }
        if (m_setLayout) {
            vkDestroyDescriptorSetLayout(
                m_device, std::exchange(m_setLayout, VK_NULL_HANDLE), nullptr);
        }
        if (m_sampler) {
            vkDestroySampler(m_device, std::exchange(m_sampler, VK_NULL_HANDLE), nullptr);
        }
    }
}

} // namespace ar::rhi::detail
