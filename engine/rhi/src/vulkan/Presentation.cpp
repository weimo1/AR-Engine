#include "Presentation.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "VulkanResult.h"
#include "rhi/RhiSession.h"

namespace ar::rhi::detail {
namespace {

constexpr std::uint64_t kFrameTimeout = 5'000'000'000;

core::Error failed(const char* operation, VkResult result) {
    return {
        core::ErrorCategory::InitializationFailed,
        core::ErrorStage::Capability,
        std::string(operation) + " returned " + vkResultName(result),
        core::ErrorOrigin{"vulkan", result},
    };
}

core::Error unavailable(const char* message) {
    return {core::ErrorCategory::QueryFailed, core::ErrorStage::Capability, message, std::nullopt};
}

template <class Handle, class Info, class Create>
std::expected<void, core::Error>
create(VkDevice device, Handle& handle, const Info& info, Create function, const char* name) {
    Handle created = VK_NULL_HANDLE;
    const VkResult result = function(device, &info, nullptr, &created);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed(name, result));
    }
    handle = created;
    return {};
}

bool presentEnqueued(VkResult result) {
    // Surface rejection still enqueues work. On device loss or an unknown result,
    // keep the fence in the terminal wait because the operation may have reached the queue.
    return result != VK_ERROR_OUT_OF_HOST_MEMORY && result != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
           result != VK_ERROR_VALIDATION_FAILED_EXT;
}

} // namespace

Presentation::Presentation(VkDevice device, VkQueue graphicsQueue, VkQueue presentQueue) noexcept
    : m_device(device), m_graphicsQueue(graphicsQueue), m_presentQueue(presentQueue) {}

std::expected<void, core::Error> Presentation::open(
    VkPhysicalDevice physical,
    VkSurfaceKHR surface,
    std::uint32_t graphicsFamily,
    std::uint32_t presentFamily,
    VkExtent2D framebuffer,
    const TriangleShaders* shaders,
    bool enableScreenshot,
    VkDescriptorSetLayout bindingLayout,
    VkDescriptorSet bindingSet) {
    if (!vkGetPhysicalDeviceSurfaceCapabilitiesKHR || !vkGetPhysicalDeviceSurfaceFormatsKHR ||
        !vkCreateSwapchainKHR || !vkDestroySwapchainKHR || !vkGetSwapchainImagesKHR ||
        !vkAcquireNextImageKHR || !vkQueuePresentKHR) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::LoaderSymbolMissing,
                core::ErrorStage::Capability,
                "Vulkan swapchain entry points are unavailable",
                std::nullopt});
    }
    m_physical = physical;
    m_surface = surface;
    m_graphicsFamily = graphicsFamily;
    m_presentFamily = presentFamily;
    m_shaders = shaders;
    m_enableScreenshot = enableScreenshot;
    m_bindingSet = bindingSet;
    for (Frame& frame : m_frames) {
        if (auto created = createFrame(frame, graphicsFamily); !created) {
            return created;
        }
    }
    if (m_shaders) {
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (bindingLayout) {
            layout.setLayoutCount = 1;
            layout.pSetLayouts = &bindingLayout;
        }
        if (auto created = create(
                m_device,
                m_pipelineLayout,
                layout,
                vkCreatePipelineLayout,
                "vkCreatePipelineLayout");
            !created) {
            return created;
        }
    }
    auto created = createSwapchain(framebuffer);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }
    if (!*created) {
        return std::unexpected(unavailable("the initial surface has no drawable extent"));
    }
    return {};
}

std::expected<bool, core::Error> Presentation::createSwapchain(VkExtent2D framebuffer) {
    VkSurfaceCapabilitiesKHR caps{};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, m_surface, &caps);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result));
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        return std::unexpected(unavailable("the surface does not support color attachments"));
    }
    m_swapchain.extent = caps.currentExtent;
    if (m_swapchain.extent.width == std::numeric_limits<std::uint32_t>::max()) {
        m_swapchain.extent = {
            std::clamp(framebuffer.width, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(framebuffer.height, caps.minImageExtent.height, caps.maxImageExtent.height),
        };
    }
    if (m_swapchain.extent.width == 0 || m_swapchain.extent.height == 0) {
        return false;
    }

    std::uint32_t count = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &count, nullptr);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkGetPhysicalDeviceSurfaceFormatsKHR", result));
    }
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (formats.empty()) {
        return std::unexpected(unavailable("the surface has no formats"));
    }
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &count, formats.data());
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkGetPhysicalDeviceSurfaceFormatsKHR", result));
    }
    formats.resize(count);
    if (formats.empty()) {
        return std::unexpected(unavailable("the surface has no formats"));
    }
    constexpr std::array preferredFormats{
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM};
    const auto format = std::ranges::find_if(preferredFormats, [&](VkFormat candidate) {
        return std::ranges::any_of(formats, [candidate](const VkSurfaceFormatKHR& surfaceFormat) {
            return surfaceFormat.format == candidate &&
                   surfaceFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
    });
    if (format == preferredFormats.end()) {
        return std::unexpected(unavailable("the surface has no supported 8-bit SDR color format"));
    }
    m_swapchain.format = *format;
    if (m_enableScreenshot && !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return std::unexpected(
            unavailable("the surface cannot copy swapchain images for screenshots"));
    }
    std::uint32_t imageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount != 0) {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }
    constexpr std::array alphaModes{
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    const auto alpha = std::ranges::find_if(alphaModes, [&](VkCompositeAlphaFlagBitsKHR mode) {
        return (caps.supportedCompositeAlpha & mode) != 0;
    });
    if (alpha == alphaModes.end()) {
        return std::unexpected(unavailable("the surface has no supported composite alpha mode"));
    }

    const std::array families{m_graphicsFamily, m_presentFamily};
    VkSwapchainCreateInfoKHR swapchain{};
    swapchain.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain.surface = m_surface;
    swapchain.minImageCount = imageCount;
    swapchain.imageFormat = *format;
    swapchain.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchain.imageExtent = m_swapchain.extent;
    swapchain.imageArrayLayers = 1;
    swapchain.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (m_enableScreenshot) {
        swapchain.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    swapchain.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (m_graphicsFamily != m_presentFamily) {
        swapchain.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain.queueFamilyIndexCount = static_cast<std::uint32_t>(families.size());
        swapchain.pQueueFamilyIndices = families.data();
    }
    swapchain.preTransform = caps.currentTransform;
    swapchain.compositeAlpha = *alpha;
    swapchain.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain.clipped = VK_TRUE;
    if (auto created = create(
            m_device, m_swapchain.handle, swapchain, vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
        !created) {
        return std::unexpected(std::move(created.error()));
    }

    result = vkGetSwapchainImagesKHR(m_device, m_swapchain.handle, &count, nullptr);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkGetSwapchainImagesKHR", result));
    }
    std::vector<VkImage> images(count);
    if (!images.empty()) {
        result = vkGetSwapchainImagesKHR(m_device, m_swapchain.handle, &count, images.data());
        if (result != VK_SUCCESS) {
            return std::unexpected(failed("vkGetSwapchainImagesKHR", result));
        }
    }
    if (count == 0) {
        return std::unexpected(unavailable("the swapchain has no images"));
    }
    m_swapchain.images.resize(count);
    for (std::size_t index = 0; index < m_swapchain.images.size(); ++index) {
        Image& image = m_swapchain.images[index];
        image.handle = images[index];
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = image.handle;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = *format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (auto created =
                create(m_device, image.view, view, vkCreateImageView, "vkCreateImageView");
            !created) {
            return std::unexpected(std::move(created.error()));
        }
    }

    if (m_enableScreenshot) {
        if (auto created = createReadback(m_physical); !created) {
            return std::unexpected(std::move(created.error()));
        }
    }
    if (m_shaders) {
        if (auto created = createTrianglePipeline(*format, *m_shaders); !created) {
            return std::unexpected(std::move(created.error()));
        }
    }
    m_requestedExtent = framebuffer;
    return true;
}

std::expected<void, core::Error> Presentation::createReadback(VkPhysicalDevice physical) {
    constexpr VkDeviceSize maxBytes = 128u * 1024u * 1024u;
    const auto pixels =
        static_cast<VkDeviceSize>(m_swapchain.extent.width) * m_swapchain.extent.height;
    if (pixels > maxBytes / 4) {
        return std::unexpected(unavailable("screenshot exceeds the 128 MiB readback limit"));
    }
    m_swapchain.readbackBytes = static_cast<std::size_t>(pixels * 4);
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = m_swapchain.readbackBytes;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (auto created =
            create(m_device, m_swapchain.readback, buffer, vkCreateBuffer, "vkCreateBuffer");
        !created) {
        return created;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, m_swapchain.readback, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        const auto flags = memory.memoryTypes[index].propertyFlags;
        if (!(requirements.memoryTypeBits & (1u << index)) ||
            !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            continue;
        }
        m_swapchain.hostCoherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = index;
        if (auto allocated = create(
                m_device,
                m_swapchain.readbackMemory,
                allocation,
                vkAllocateMemory,
                "vkAllocateMemory");
            !allocated) {
            return allocated;
        }
        const VkResult result =
            vkBindBufferMemory(m_device, m_swapchain.readback, m_swapchain.readbackMemory, 0);
        if (result != VK_SUCCESS) {
            return std::unexpected(failed("vkBindBufferMemory", result));
        }
        return {};
    }
    return std::unexpected(unavailable("no host-visible memory type for the screenshot buffer"));
}

std::expected<void, core::Error>
Presentation::readScreenshot(Frame& frame, FrameScreenshot& screenshot) {
    VkResult result = vkWaitForFences(m_device, 1, &frame.submitted.handle, VK_TRUE, kFrameTimeout);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("waiting for screenshot submission", result));
    }
    frame.submitted.pending = false;
    FrameScreenshot captured{
        m_swapchain.extent.width,
        m_swapchain.extent.height,
        std::vector<std::uint8_t>(m_swapchain.readbackBytes)};
    void* mapped = nullptr;
    result = vkMapMemory(m_device, m_swapchain.readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkMapMemory", result));
    }
    if (!m_swapchain.hostCoherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = m_swapchain.readbackMemory;
        range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(m_device, 1, &range);
    }
    if (result == VK_SUCCESS) {
        std::memcpy(captured.rgba.data(), mapped, captured.rgba.size());
    }
    vkUnmapMemory(m_device, m_swapchain.readbackMemory);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkInvalidateMappedMemoryRanges", result));
    }
    if (m_swapchain.format == VK_FORMAT_B8G8R8A8_SRGB ||
        m_swapchain.format == VK_FORMAT_B8G8R8A8_UNORM) {
        for (std::size_t i = 0; i < captured.rgba.size(); i += 4) {
            std::swap(captured.rgba[i], captured.rgba[i + 2]);
        }
    }
    screenshot = std::move(captured);
    return {};
}

std::expected<void, core::Error>
Presentation::createTrianglePipeline(VkFormat format, const TriangleShaders& shaders) {
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

    const std::array bytecode{&shaders.vertex, &shaders.fragment};
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
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{
        0.0F,
        0.0F,
        static_cast<float>(m_swapchain.extent.width),
        static_cast<float>(m_swapchain.extent.height),
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
    blend.attachmentCount = 1;
    blend.pAttachments = &color;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.pNext = &rendering;
    pipeline.stageCount = static_cast<std::uint32_t>(stageInfos.size());
    pipeline.pStages = stageInfos.data();
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewportState;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    pipeline.layout = m_pipelineLayout;
    const VkResult result = vkCreateGraphicsPipelines(
        m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_swapchain.pipeline);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateGraphicsPipelines", result));
    }
    return {};
}

std::expected<void, core::Error>
Presentation::createFrame(Frame& frame, std::uint32_t graphicsFamily) {
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = graphicsFamily;
    if (auto created =
            create(m_device, frame.pool, pool, vkCreateCommandPool, "vkCreateCommandPool");
        !created) {
        return created;
    }
    VkCommandBufferAllocateInfo commands{};
    commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commands.commandPool = frame.pool;
    commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands.commandBufferCount = 1;
    const VkResult result = vkAllocateCommandBuffers(m_device, &commands, &frame.commands);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkAllocateCommandBuffers", result));
    }
    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (VkSemaphore* handle : {&frame.imageAvailable, &frame.renderingFinished}) {
        if (auto created =
                create(m_device, *handle, semaphore, vkCreateSemaphore, "vkCreateSemaphore");
            !created) {
            return created;
        }
    }
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (Fence* slot : {&frame.acquired, &frame.submitted, &frame.presented}) {
        if (auto created = create(m_device, slot->handle, fence, vkCreateFence, "vkCreateFence");
            !created) {
            return created;
        }
    }
    return {};
}

std::expected<void, core::Error>
Presentation::record(VkCommandBuffer commands, const Image& image, bool capture) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult result = vkBeginCommandBuffer(commands, &begin);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkBeginCommandBuffer", result));
    }

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    // Acquire is waited at COLOR_ATTACHMENT_OUTPUT, including the layout transition.
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.handle;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = image.view;
    color.imageLayout = barrier.newLayout;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.025F, 0.06F, 0.14F, 1.0F}};
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = m_swapchain.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(commands, &rendering);
    if (m_swapchain.pipeline) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_swapchain.pipeline);
        if (m_bindingSet) {
            vkCmdBindDescriptorSets(
                commands,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_pipelineLayout,
                0,
                1,
                &m_bindingSet,
                0,
                nullptr);
        }
        vkCmdDraw(commands, 3, 1, 0, 0);
    }
    vkCmdEndRendering(commands);

    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstAccessMask = VK_ACCESS_2_NONE;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier2 host{};
    if (capture) {
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier2(commands, &dependency);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_swapchain.extent.width, m_swapchain.extent.height, 1};
        vkCmdCopyImageToBuffer(
            commands, image.handle, barrier.newLayout, m_swapchain.readback, 1, &region);

        host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = m_swapchain.readback;
        host.size = VK_WHOLE_SIZE;
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &host;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask = VK_ACCESS_2_NONE;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    vkCmdPipelineBarrier2(commands, &dependency);
    result = vkEndCommandBuffer(commands);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkEndCommandBuffer", result));
    }
    return {};
}

VkResult Presentation::drain(Frame& frame, std::uint64_t timeout) {
    std::array<VkFence, 3> handles{};
    std::uint32_t count = 0;
    for (const Fence* fence : {&frame.acquired, &frame.submitted, &frame.presented}) {
        if (fence->pending) {
            handles[count++] = fence->handle;
        }
    }
    if (count == 0) {
        return VK_SUCCESS;
    }
    const VkResult result = vkWaitForFences(m_device, count, handles.data(), VK_TRUE, timeout);
    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
        return result;
    }
    for (Fence* fence : {&frame.acquired, &frame.submitted, &frame.presented}) {
        fence->pending = false;
    }
    // A device-lost result retires these fences for pending/in-use checks, but still reports loss.
    return result;
}

std::expected<Presentation::DrawStatus, core::Error>
Presentation::draw(Frame& frame, bool capture) {
    VkResult result = drain(frame, kFrameTimeout);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("waiting for the frame fences", result));
    }
    result = vkResetCommandPool(m_device, frame.pool, 0);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkResetCommandPool", result));
    }
    const std::array fences{frame.acquired.handle, frame.submitted.handle, frame.presented.handle};
    result = vkResetFences(m_device, static_cast<std::uint32_t>(fences.size()), fences.data());
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkResetFences", result));
    }
    std::uint32_t imageIndex = 0;
    result = vkAcquireNextImageKHR(
        m_device,
        m_swapchain.handle,
        kFrameTimeout,
        frame.imageAvailable,
        frame.acquired.handle,
        &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return DrawStatus::OutOfDate;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return std::unexpected(failed("vkAcquireNextImageKHR", result));
    }
    const bool suboptimalAcquire = result == VK_SUBOPTIMAL_KHR;
    frame.acquired.pending = true;
    if (auto recorded = record(frame.commands, m_swapchain.images[imageIndex], capture);
        !recorded) {
        return std::unexpected(std::move(recorded.error()));
    }

    VkSemaphoreSubmitInfo acquire{};
    acquire.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    acquire.semaphore = frame.imageAvailable;
    acquire.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo render{};
    render.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    render.semaphore = frame.renderingFinished;
    render.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo command{};
    command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command.commandBuffer = frame.commands;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &acquire;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &render;
    result = vkQueueSubmit2(m_graphicsQueue, 1, &submit, frame.submitted.handle);
    frame.submitted.pending = result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkQueueSubmit2", result));
    }

    VkSwapchainPresentFenceInfoKHR presentFence{};
    presentFence.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR;
    presentFence.swapchainCount = 1;
    presentFence.pFences = &frame.presented.handle;
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.pNext = &presentFence;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &frame.renderingFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &m_swapchain.handle;
    present.pImageIndices = &imageIndex;
    result = vkQueuePresentKHR(m_presentQueue, &present);
    frame.presented.pending = presentEnqueued(result);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return DrawStatus::OutOfDate;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return std::unexpected(failed("vkQueuePresentKHR", result));
    }
    return suboptimalAcquire || result == VK_SUBOPTIMAL_KHR ? DrawStatus::Suboptimal
                                                            : DrawStatus::Presented;
}

std::expected<PresentStatus, core::Error>
Presentation::presentFrame(VkExtent2D framebuffer, FrameScreenshot* screenshot) {
    if (m_failed || (screenshot && !m_enableScreenshot)) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "the swapchain cannot accept another frame",
                std::nullopt});
    }
    if (framebuffer.width == 0 || framebuffer.height == 0) {
        return PresentStatus::Retry;
    }
    if (!m_swapchain.handle || m_rebuild || framebuffer.width != m_requestedExtent.width ||
        framebuffer.height != m_requestedExtent.height) {
        if (auto error = drainFrames()) {
            m_failed = true;
            return std::unexpected(std::move(*error));
        }
        destroySwapchain();
        auto created = createSwapchain(framebuffer);
        if (!created) {
            m_failed = true;
            return std::unexpected(std::move(created.error()));
        }
        if (!*created) {
            return PresentStatus::Retry;
        }
        m_rebuild = false;
    }
    auto& frame = m_frames[m_frameIndex];
    auto result = draw(frame, screenshot != nullptr);
    if (!result) {
        m_failed = true;
        return std::unexpected(std::move(result.error()));
    }
    m_rebuild = *result != DrawStatus::Presented;
    if (*result == DrawStatus::OutOfDate) {
        return PresentStatus::Retry;
    }
    if (screenshot) {
        if (auto captured = readScreenshot(frame, *screenshot); !captured) {
            m_failed = true;
            return std::unexpected(std::move(captured.error()));
        }
    }
    m_frameIndex = (m_frameIndex + 1) % m_frames.size();
    return PresentStatus::Presented;
}

std::optional<core::Error> Presentation::drainFrames() {
    std::optional<core::Error> error;
    for (Frame& frame : m_frames) {
        const VkResult result = drain(frame, kFrameTimeout);
        if (result == VK_SUCCESS) {
            continue;
        }
        error = failed("draining the frame fences", result);
        error->category = core::ErrorCategory::CleanupFailed;
        error->stage = core::ErrorStage::Cleanup;
        if (result != VK_ERROR_DEVICE_LOST) {
            // The parent quarantines the process and preserves the complete lifetime chain.
            return error;
        }
    }
    return error;
}

std::optional<core::Error> Presentation::close() {
    auto error = drainFrames();
    if (error && (!error->origin || error->origin->code != VK_ERROR_DEVICE_LOST)) {
        return error;
    }
    for (Frame& frame : m_frames) {
        if (frame.pool) {
            vkDestroyCommandPool(m_device, frame.pool, nullptr);
        }
        for (VkSemaphore handle : {frame.imageAvailable, frame.renderingFinished}) {
            if (handle) {
                vkDestroySemaphore(m_device, handle, nullptr);
            }
        }
        for (Fence fence : {frame.acquired, frame.submitted, frame.presented}) {
            if (fence.handle) {
                vkDestroyFence(m_device, fence.handle, nullptr);
            }
        }
        frame = {};
    }
    destroySwapchain();
    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(m_device, std::exchange(m_pipelineLayout, VK_NULL_HANDLE), nullptr);
    }
    return error;
}

void Presentation::destroySwapchain() noexcept {
    for (const Image& image : m_swapchain.images) {
        if (image.view) {
            vkDestroyImageView(m_device, image.view, nullptr);
        }
    }
    if (m_swapchain.pipeline) {
        vkDestroyPipeline(m_device, std::exchange(m_swapchain.pipeline, VK_NULL_HANDLE), nullptr);
    }
    if (m_swapchain.readback) {
        vkDestroyBuffer(m_device, m_swapchain.readback, nullptr);
    }
    if (m_swapchain.readbackMemory) {
        vkFreeMemory(m_device, m_swapchain.readbackMemory, nullptr);
    }
    if (m_swapchain.handle) {
        vkDestroySwapchainKHR(m_device, m_swapchain.handle, nullptr);
    }
    m_swapchain = {};
}

} // namespace ar::rhi::detail
