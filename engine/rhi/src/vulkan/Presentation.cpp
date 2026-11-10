#include "Presentation.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "VulkanResult.h"

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
    VkExtent2D framebuffer) {
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

    VkSurfaceCapabilitiesKHR caps{};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result));
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        return std::unexpected(unavailable("the surface does not support color attachments"));
    }
    m_extent = caps.currentExtent;
    if (m_extent.width == std::numeric_limits<std::uint32_t>::max()) {
        m_extent = {
            std::clamp(framebuffer.width, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(framebuffer.height, caps.minImageExtent.height, caps.maxImageExtent.height),
        };
    }
    if (m_extent.width == 0 || m_extent.height == 0) {
        return std::unexpected(unavailable("the surface has no drawable extent"));
    }

    std::uint32_t count = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkGetPhysicalDeviceSurfaceFormatsKHR", result));
    }
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (formats.empty()) {
        return std::unexpected(unavailable("the surface has no formats"));
    }
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data());
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

    const std::array families{graphicsFamily, presentFamily};
    VkSwapchainCreateInfoKHR swapchain{};
    swapchain.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain.surface = surface;
    swapchain.minImageCount = imageCount;
    swapchain.imageFormat = *format;
    swapchain.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchain.imageExtent = m_extent;
    swapchain.imageArrayLayers = 1;
    swapchain.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (graphicsFamily != presentFamily) {
        swapchain.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain.queueFamilyIndexCount = static_cast<std::uint32_t>(families.size());
        swapchain.pQueueFamilyIndices = families.data();
    }
    swapchain.preTransform = caps.currentTransform;
    swapchain.compositeAlpha = *alpha;
    swapchain.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain.clipped = VK_TRUE;
    if (auto created =
            create(m_device, m_swapchain, swapchain, vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
        !created) {
        return created;
    }

    result = vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkGetSwapchainImagesKHR", result));
    }
    std::vector<VkImage> images(count);
    if (!images.empty()) {
        result = vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, images.data());
        if (result != VK_SUCCESS) {
            return std::unexpected(failed("vkGetSwapchainImagesKHR", result));
        }
    }
    if (count == 0) {
        return std::unexpected(unavailable("the swapchain has no images"));
    }
    m_images.resize(count);
    for (std::size_t index = 0; index < m_images.size(); ++index) {
        Image& image = m_images[index];
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
            return created;
        }
    }

    for (Frame& frame : m_frames) {
        if (auto created = createFrame(frame, graphicsFamily); !created) {
            return created;
        }
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
Presentation::record(VkCommandBuffer commands, const Image& image) {
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
    rendering.renderArea.extent = m_extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(commands, &rendering);
    vkCmdEndRendering(commands);

    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstAccessMask = VK_ACCESS_2_NONE;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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

std::expected<void, core::Error> Presentation::draw(Frame& frame) {
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
        m_swapchain,
        kFrameTimeout,
        frame.imageAvailable,
        frame.acquired.handle,
        &imageIndex);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return std::unexpected(failed("vkAcquireNextImageKHR", result));
    }
    frame.acquired.pending = true;
    if (auto recorded = record(frame.commands, m_images[imageIndex]); !recorded) {
        return recorded;
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
    present.pSwapchains = &m_swapchain;
    present.pImageIndices = &imageIndex;
    result = vkQueuePresentKHR(m_presentQueue, &present);
    frame.presented.pending = presentEnqueued(result);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return std::unexpected(failed("vkQueuePresentKHR", result));
    }
    return {};
}

std::expected<void, core::Error> Presentation::presentClearFrame() {
    if (m_failed || !m_swapchain) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "the swapchain cannot accept another frame",
                std::nullopt});
    }
    auto result = draw(m_frames[m_frameIndex]);
    if (!result) {
        m_failed = true;
        return result;
    }
    m_frameIndex = (m_frameIndex + 1) % m_frames.size();
    return {};
}

std::optional<core::Error> Presentation::close() {
    if (!m_swapchain) {
        return std::nullopt;
    }
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
    }
    for (const Image& image : m_images) {
        if (image.view) {
            vkDestroyImageView(m_device, image.view, nullptr);
        }
    }
    vkDestroySwapchainKHR(m_device, std::exchange(m_swapchain, VK_NULL_HANDLE), nullptr);
    return error;
}

} // namespace ar::rhi::detail
