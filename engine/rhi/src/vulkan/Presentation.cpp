#include "Presentation.h"
#include "resources/DeviceResources.h"
#include "core/Diagnostics.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "VulkanResult.h"
#include "rhi/RhiSession.h"

namespace ar::rhi::detail {
namespace {

constexpr std::uint64_t kFrameTimeout = 5'000'000'000;
constexpr std::array gbufferFormats{
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8_UNORM};
constexpr VkFormat hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkDeviceSize kProbeStride = 32;
constexpr std::array<VkDeviceSize, 5> probeOffsets{0, 8, 16, 20, 24};

constexpr VkDeviceSize probeStart(VkDeviceSize screenshotBytes) {
    return (screenshotBytes + 7) & ~VkDeviceSize{7};
}

std::array<VkOffset3D, 3> probePositions(VkExtent2D extent) {
    const auto coordinate = [](std::uint32_t size, float fraction) {
        return static_cast<std::int32_t>(static_cast<float>(size) * fraction);
    };
    return {{
        {coordinate(extent.width, 0.5F), coordinate(extent.height, 0.38F), 0},
        {coordinate(extent.width, 0.5F), coordinate(extent.height, 0.76F), 0},
        {coordinate(extent.width, 0.05F), coordinate(extent.height, 0.05F), 0},
    }};
}

float halfToFloat(std::uint16_t bits) {
    const int exponent = (bits >> 10) & 31;
    const auto fraction = static_cast<float>(bits & 1023);
    float magnitude = 0.F;
    if (exponent == 0) {
        magnitude = std::ldexp(fraction, -24);
    } else if (exponent == 31) {
        magnitude = fraction == 0 ? std::numeric_limits<float>::infinity()
                                  : std::numeric_limits<float>::quiet_NaN();
    } else {
        magnitude = std::ldexp(1.F + fraction / 1024.F, exponent - 15);
    }
    return (bits & 0x8000u) ? -magnitude : magnitude;
}

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
    VkDescriptorSet bindingSet,
    StaticSceneDraw* staticScene,
    bool enableGBuffer,
    const DeferredLighting* lighting,
    PlanPresentationGraph planner) {
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
    m_staticScene = staticScene;
    m_enableGBuffer = enableGBuffer;
    m_planPresentation = planner;
    m_lighting = lighting;
    for (Frame& frame : m_frames) {
        if (auto created = createFrame(frame, graphicsFamily); !created) {
            return created;
        }
    }
    if (m_shaders) {
        const auto indirectLayout =
            m_staticScene ? m_staticScene->indirectLayout() : VK_NULL_HANDLE;
        const std::array setLayouts{bindingLayout, indirectLayout};
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (bindingLayout) {
            layout.setLayoutCount = indirectLayout ? 2u : 1u;
            layout.pSetLayouts = setLayouts.data();
        }
        const VkPushConstantRange matrix{
            m_enableGBuffer && !indirectLayout
                ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                : static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT),
            0,
            indirectLayout    ? 4u
            : m_enableGBuffer ? 128u
                              : 64u};
        if (m_staticScene) {
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &matrix;
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
    if (m_lighting) {
        if (auto created = createLightingLayout(); !created) {
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
        for (auto& readback : m_swapchain.readbacks) {
            if (auto created = createReadback(m_physical, readback); !created) {
                return std::unexpected(std::move(created.error()));
            }
        }
    }
    if (m_staticScene) {
        for (auto& slot : m_swapchain.attachments) {
            const VkImageUsageFlags readUsage = VK_IMAGE_USAGE_SAMPLED_BIT |
                (m_enableScreenshot ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT) : 0);
            auto created = m_staticScene->createAttachment(
                m_swapchain.extent, StaticSceneDraw::depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | (m_enableGBuffer ? readUsage : 0));
            if (!created) {
                return std::unexpected(created.error());
            }
            slot.depth = *created;
            if (m_enableGBuffer) {
                for (std::size_t index = 0; index < gbufferFormats.size(); ++index) {
                    auto attachment = m_staticScene->createAttachment(
                        m_swapchain.extent, gbufferFormats[index], VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | readUsage);
                    if (!attachment) {
                        return std::unexpected(attachment.error());
                    }
                    slot.gbuffer[index] = *attachment;
                }
            }
        }
    }
    if (m_shaders) {
        const VkFormat geometryOutput = m_lighting ? VK_FORMAT_UNDEFINED : *format;
        if (auto created = createGraphicsPipeline(
                geometryOutput, m_shaders->vertex, m_shaders->fragment, m_swapchain.pipeline);
            !created) {
            return std::unexpected(std::move(created.error()));
        }
    }
    if (m_lighting) {
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (m_enableScreenshot) {
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        if (auto created = createLightingResources(usage); !created) {
            return std::unexpected(created.error());
        }
        if (auto created = createGraphicsPipeline(
                hdrFormat, m_lighting->vertex, m_lighting->directFragment, m_swapchain.directPipeline, true);
            !created) {
            return std::unexpected(created.error());
        }
        if (auto created = createGraphicsPipeline(
                *format, m_lighting->vertex, m_lighting->displayFragment, m_swapchain.displayPipeline, true);
            !created) {
            return std::unexpected(created.error());
        }
    }
    m_requestedExtent = framebuffer;
    return true;
}

std::expected<void, core::Error> Presentation::createLightingLayout() {
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (auto created = create(m_device, m_lightingSampler, sampler, vkCreateSampler, "lighting sampler");
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
    if (auto created = create(m_device, m_lightingSetLayout, set, vkCreateDescriptorSetLayout,
                              "lighting descriptor layout"); !created) {
        return created;
    }
    const VkPushConstantRange push{
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        static_cast<std::uint32_t>(sizeof(m_inverseProjection) + sizeof(m_viewToWorld))};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m_lightingSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    return create(m_device, m_lightingLayout, layout, vkCreatePipelineLayout, "lighting pipeline layout");
}

std::expected<void, core::Error> Presentation::createLightingResources(VkImageUsageFlags usage) {
    constexpr std::uint32_t setsPerSlot = 2;
    constexpr std::uint32_t gbufferBindings = 4;
    const std::uint32_t bindingsPerSet = m_lighting->environmentLighting ? 7u : gbufferBindings;
    const auto setCount = setsPerSlot * static_cast<std::uint32_t>(m_swapchain.attachments.size());
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount * bindingsPerSet};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = setCount;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (auto created = create(m_device, m_swapchain.lightingPool, pool, vkCreateDescriptorPool,
                              "lighting descriptor pool"); !created) {
        return created;
    }

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
        const std::array views{slot.gbuffer[0].view, slot.gbuffer[1].view, slot.gbuffer[2].view,
                               slot.depth.view, slot.hdr.view};
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
        vkUpdateDescriptorSets(m_device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
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
    VkCommandBuffer commands, const Image& output, const PresentationGraphPlan* plan) const {
    const auto& slot = m_swapchain.attachments[m_frameIndex];
    const auto transition = [](VkImage image, VkImageAspectFlags aspect,
                               VkPipelineStageFlags2 fromStage, VkAccessFlags2 fromAccess,
                               VkImageLayout fromLayout, VkPipelineStageFlags2 toStage,
                               VkAccessFlags2 toAccess, VkImageLayout toLayout) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = fromStage;
        barrier.srcAccessMask = fromAccess;
        barrier.dstStageMask = toStage;
        barrier.dstAccessMask = toAccess;
        barrier.oldLayout = fromLayout;
        barrier.newLayout = toLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        return barrier;
    };
    constexpr auto colorStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr auto colorWrite = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    constexpr auto colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    constexpr auto sampleStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    constexpr auto sampleRead = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    constexpr auto sampleLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const auto colorToSample = [&](VkImage image) {
        return transition(image, VK_IMAGE_ASPECT_COLOR_BIT, colorStage, colorWrite, colorLayout,
                          sampleStage, sampleRead, sampleLayout);
    };

    const std::array barriers{
        colorToSample(slot.gbuffer[0].image),
        colorToSample(slot.gbuffer[1].image),
        colorToSample(slot.gbuffer[2].image),
        transition(slot.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   sampleStage, sampleRead, sampleLayout),
        transition(slot.hdr.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                   VK_IMAGE_LAYOUT_UNDEFINED, colorStage, colorWrite, colorLayout),
    };
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers = barriers.data();
    if (plan) {
        recordGraphBarriers(commands, output, *plan, PresentationPass::Lighting);
    } else {
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    const auto draw = [&](VkImageView target, VkPipeline pipeline, VkDescriptorSet set,
                          std::uint32_t size, const void* push) {
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = target;
        color.imageLayout = colorLayout;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};
        render.renderArea.extent = m_swapchain.extent;
        render.layerCount = 1;
        render.colorAttachmentCount = 1;
        render.pColorAttachments = &color;
        vkCmdBeginRendering(commands, &render);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingLayout,
                                0, 1, &set, 0, nullptr);
        vkCmdPushConstants(commands, m_lightingLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, push);
        vkCmdDraw(commands, 3, 1, 0, 0);
        vkCmdEndRendering(commands);
    };

    auto inverse = m_inverseProjection;
    inverse[0] *= static_cast<float>(m_swapchain.extent.width) /
                  static_cast<float>(m_swapchain.extent.height);
    const struct DirectPush {
        std::array<float, 16> inverseProjection;
        std::array<float, 12> viewToWorld;
    } direct{inverse, m_viewToWorld};
    static_assert(sizeof(DirectPush) == 112);
    draw(slot.hdr.view, m_swapchain.directPipeline, slot.directSet, sizeof(direct), &direct);

    const auto hdrBarrier = colorToSample(slot.hdr.image);
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &hdrBarrier;
    if (plan) {
        recordGraphBarriers(commands, output, *plan, PresentationPass::Display);
    } else {
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    const bool srgb = m_swapchain.format == VK_FORMAT_B8G8R8A8_SRGB ||
                      m_swapchain.format == VK_FORMAT_R8G8B8A8_SRGB;
    const struct DisplayPush {
        float exposureEV;
        std::uint32_t encodeSrgb;
    } display{m_lighting->exposureEV, srgb ? 0u : 1u};
    static_assert(sizeof(DisplayPush) == 8);
    draw(output.view, m_swapchain.displayPipeline, slot.displaySet, sizeof(display), &display);
}

std::expected<void, core::Error>
Presentation::createReadback(VkPhysicalDevice physical, Readback& readback) {
    constexpr VkDeviceSize maxBytes = 128u * 1024u * 1024u;
    const auto pixels =
        static_cast<VkDeviceSize>(m_swapchain.extent.width) * m_swapchain.extent.height;
    if (pixels > maxBytes / 4) {
        return std::unexpected(unavailable("screenshot exceeds the 128 MiB readback limit"));
    }
    m_swapchain.readbackBytes = static_cast<std::size_t>(pixels * 4);
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = m_enableGBuffer
        ? probeStart(m_swapchain.readbackBytes) + kProbeStride * probePositions(m_swapchain.extent).size()
        : m_swapchain.readbackBytes;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (auto created = create(m_device, readback.buffer, buffer, vkCreateBuffer, "vkCreateBuffer");
        !created) {
        return created;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, readback.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        const auto flags = memory.memoryTypes[index].propertyFlags;
        if (!(requirements.memoryTypeBits & (1u << index)) ||
            !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            continue;
        }
        readback.coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = index;
        if (auto allocated =
                create(m_device, readback.memory, allocation, vkAllocateMemory, "vkAllocateMemory");
            !allocated) {
            return allocated;
        }
        const VkResult result = vkBindBufferMemory(m_device, readback.buffer, readback.memory, 0);
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
    const auto& readback = m_swapchain.readbacks[&frame - m_frames.data()];
    FrameScreenshot captured{
        m_swapchain.extent.width,
        m_swapchain.extent.height,
        std::vector<std::uint8_t>(m_swapchain.readbackBytes)};
    void* mapped = nullptr;
    result = vkMapMemory(m_device, readback.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkMapMemory", result));
    }
    if (!readback.coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = readback.memory;
        range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(m_device, 1, &range);
    }
    if (result == VK_SUCCESS) {
        std::memcpy(captured.rgba.data(), mapped, captured.rgba.size());
        if (m_enableGBuffer) {
            decodeGBufferProbes(mapped, captured);
        }
    }
    vkUnmapMemory(m_device, readback.memory);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkInvalidateMappedMemoryRanges", result));
    }
    if (m_swapchain.format == VK_FORMAT_B8G8R8A8_SRGB ||
        m_swapchain.format == VK_FORMAT_B8G8R8A8_UNORM) {
        for (std::size_t i = 0; i < captured.rgba.size(); i += 4) {
            std::swap(captured.rgba[i], captured.rgba[i + 2]);
        }
    }
    if (m_staticScene) {
        if (auto visibility = m_staticScene->readVisibility(&frame - m_frames.data(), captured);
            !visibility) {
            return visibility;
        }
    }
    screenshot = std::move(captured);
    return {};
}

void Presentation::decodeGBufferProbes(const void* mapped, FrameScreenshot& screenshot) const {
    const auto* samples = static_cast<const std::uint8_t*>(mapped) + probeStart(m_swapchain.readbackBytes);
    const auto positions = probePositions(m_swapchain.extent);
    screenshot.gbuffer.resize(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        auto& output = screenshot.gbuffer[i];
        output.pixel = {static_cast<std::uint32_t>(positions[i].x),
                        static_cast<std::uint32_t>(positions[i].y)};
        const auto* sample = samples + i * kProbeStride;
        for (std::size_t channel = 0; channel < output.baseColor.size(); ++channel) {
            output.baseColor[channel] = static_cast<float>(sample[channel]) / 255.F;
        }
        for (std::size_t channel = 0; channel < output.normal.size(); ++channel) {
            std::uint16_t bits = 0;
            std::memcpy(&bits, sample + probeOffsets[1] + channel * sizeof(bits), sizeof(bits));
            output.normal[channel] = halfToFloat(bits);
        }
        for (std::size_t channel = 0; channel < output.material.size(); ++channel) {
            output.material[channel] = static_cast<float>(sample[probeOffsets[2] + channel]) / 255.F;
        }
        std::memcpy(&output.depth, sample + probeOffsets[3], sizeof(output.depth));
        if (m_lighting) {
            auto& rgb = output.hdrColor.emplace();
            for (std::size_t channel = 0; channel < rgb.size(); ++channel) {
                std::uint16_t bits = 0;
                std::memcpy(&bits, sample + probeOffsets[4] + channel * sizeof(bits), sizeof(bits));
                rgb[channel] = halfToFloat(bits);
            }
        }
    }
}

void Presentation::copyGBufferProbes(
    VkCommandBuffer commands, VkBuffer destination, bool graph) const {
    const auto& slot = m_swapchain.attachments[m_frameIndex];
    const std::array images{slot.gbuffer[0].image, slot.gbuffer[1].image,
                            slot.gbuffer[2].image, slot.depth.image, slot.hdr.image};
    const std::uint32_t count = m_lighting ? 5 : 4;
    std::array<VkImageMemoryBarrier2, 5> transitions{};
    for (std::size_t i = 0; i < count; ++i) {
        const bool depth = i == 3;
        auto& barrier = transitions[i];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = depth ? VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                           VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                                     : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = depth ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                     : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                                  : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (m_lighting) {
            barrier.srcStageMask |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[i];
        const auto aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange = {static_cast<VkImageAspectFlags>(aspect), 0, 1, 0, 1};
    }
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = count;
    dependency.pImageMemoryBarriers = transitions.data();
    if (!graph) {
        vkCmdPipelineBarrier2(commands, &dependency);
    }
    const auto positions = probePositions(m_swapchain.extent);
    for (std::size_t i = 0; i < count; ++i) {
        std::array<VkBufferImageCopy, 3> copies{};
        for (std::size_t j = 0; j < copies.size(); ++j) {
            auto& copy = copies[j];
            copy.bufferOffset = probeStart(m_swapchain.readbackBytes) + j * kProbeStride + probeOffsets[i];
            copy.imageSubresource = {transitions[i].subresourceRange.aspectMask, 0, 0, 1};
            copy.imageOffset = positions[j];
            copy.imageExtent = {1, 1, 1};
        }
        vkCmdCopyImageToBuffer(commands, images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                               static_cast<std::uint32_t>(copies.size()), copies.data());
    }
}

std::expected<void, core::Error>
Presentation::createGraphicsPipeline(
    VkFormat format, const std::vector<std::uint32_t>& vertexCode,
    const std::vector<std::uint32_t>& fragmentCode, VkPipeline& output, bool fullscreen) {
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
    const VkVertexInputBindingDescription positions{0, 3 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription position{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    const VkVertexInputBindingDescription meshBinding{0, sizeof(StaticVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array meshAttributes{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(StaticVertex, position)},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(StaticVertex, uv)},
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(StaticVertex, normal)}};
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
    const std::array formats{format, gbufferFormats[0], gbufferFormats[1], gbufferFormats[2]};
    rendering.colorAttachmentCount = blend.attachmentCount;
    rendering.pColorAttachmentFormats = formats.data();
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (geometry) {
        rendering.depthAttachmentFormat = StaticSceneDraw::depthFormat;
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_GREATER;
    }
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
    if (geometry) {
        pipeline.pDepthStencilState = &depth;
    }
    pipeline.layout = fullscreen ? m_lightingLayout : m_pipelineLayout;
    const VkResult result = vkCreateGraphicsPipelines(
        m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &output);
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

std::expected<void, core::Error> Presentation::record(
    VkCommandBuffer commands,
    const Image& image,
    VkBuffer capture,
    const PresentationGraphPlan* plan) {
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
    if (plan) {
        recordGraphBarriers(commands, image, *plan, PresentationPass::Geometry);
        dependency.imageMemoryBarrierCount = 0;
        dependency.pImageMemoryBarriers = nullptr;
    } else {
        vkCmdPipelineBarrier2(commands, &dependency);
    }

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
    std::array<VkRenderingAttachmentInfo, 4> colors{};
    colors[0] = color;
    if (m_lighting) {
        colors[0].imageView = VK_NULL_HANDLE;
    }
    rendering.pColorAttachments = colors.data();
    if (m_enableGBuffer) {
        std::array<VkImageMemoryBarrier2, 3> transitions{};
        const auto& attachments = m_swapchain.attachments[m_frameIndex].gbuffer;
        for (std::size_t index = 0; index < attachments.size(); ++index) {
            auto& transition = transitions[index];
            transition = barrier;
            transition.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                      VK_PIPELINE_STAGE_2_COPY_BIT;
            transition.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            transition.srcAccessMask =
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
            transition.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            transition.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            transition.image = attachments[index].image;
            auto& target = colors[index + 1];
            target = color;
            target.imageView = attachments[index].view;
            target.clearValue = {};
        }
        VkDependencyInfo gbufferDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        gbufferDependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(transitions.size());
        gbufferDependency.pImageMemoryBarriers = transitions.data();
        if (!plan) {
            vkCmdPipelineBarrier2(commands, &gbufferDependency);
        }
        rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
    }
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (m_staticScene) {
        const auto& depth = m_swapchain.attachments[m_frameIndex].depth;
        VkImageMemoryBarrier2 transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        transition.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        transition.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        transition.dstStageMask = transition.srcStageMask;
        if (m_enableGBuffer && m_enableScreenshot) {
            transition.srcStageMask |= VK_PIPELINE_STAGE_2_COPY_BIT;
            transition.srcAccessMask |= VK_ACCESS_2_TRANSFER_READ_BIT;
        }
        transition.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transition.image = depth.image;
        transition.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkDependencyInfo depthDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depthDependency.imageMemoryBarrierCount = 1;
        depthDependency.pImageMemoryBarriers = &transition;
        if (!plan) {
            vkCmdPipelineBarrier2(commands, &depthDependency);
        }
        depthAttachment.imageView = depth.view;
        depthAttachment.imageLayout = transition.newLayout;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = m_enableGBuffer ? VK_ATTACHMENT_STORE_OP_STORE
                                                  : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {0.F, 0};
        rendering.pDepthAttachment = &depthAttachment;
    }
    const auto* cull = m_cullFrames[m_frameIndex];
    if (cull) {
        cull->recordCull(commands, m_cullObjects, true);
    }
    const float inverseAspect = static_cast<float>(m_swapchain.extent.height) /
                                static_cast<float>(m_swapchain.extent.width);
    if (m_staticScene && plan) {
        m_staticScene->cull(commands, m_frameIndex, inverseAspect, *plan);
    }
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
        if (cull) {
            cull->drawCull(commands);
        } else if (m_staticScene) {
            m_staticScene->record(commands, m_pipelineLayout, inverseAspect, m_frameIndex);
        } else {
            vkCmdDraw(commands, 3, 1, 0, 0);
        }
    }
    vkCmdEndRendering(commands);
    if (capture && m_staticScene && plan) {
        m_staticScene->indirectReadback(commands, m_frameIndex, *plan);
    }
    if (m_lighting) {
        recordLighting(commands, image, plan);
    }

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
        if (plan) {
            recordGraphBarriers(commands, image, *plan, PresentationPass::Copy);
        } else {
            vkCmdPipelineBarrier2(commands, &dependency);
        }
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_swapchain.extent.width, m_swapchain.extent.height, 1};
        vkCmdCopyImageToBuffer(commands, image.handle, barrier.newLayout, capture, 1, &region);
        if (m_enableGBuffer) {
            copyGBufferProbes(commands, capture, plan != nullptr);
        }

        host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = capture;
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
    if (plan) {
        recordGraphBarriers(commands, image, *plan, PresentationPass::Present);
    }
    if (!plan || capture) {
        vkCmdPipelineBarrier2(commands, &dependency);
    }
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
    auto& image = m_swapchain.images[imageIndex];
    auto& slot = m_swapchain.attachments[m_frameIndex];
    std::optional<PresentationGraphPlan> graph;
    if (m_planPresentation && m_enableGBuffer) {
        auto states = slot.graphStates;
        states[std::to_underlying(PresentationImage::Output)] = image.state;
        auto planned = m_planPresentation(
            {states,
             m_lighting != nullptr,
             capture,
             m_environmentReady,
             m_staticScene && m_staticScene->indirectEnabled()});
        if (!planned) {
            return std::unexpected(std::move(planned.error()));
        }
        if (!validGraphPlan(*planned, capture)) {
            return std::unexpected(
                unavailable("presentation graph contains an invalid resource binding or barrier"));
        }
        graph.emplace(std::move(*planned));
    }
    if (auto recorded = record(
            frame.commands,
            image,
            capture ? m_swapchain.readbacks[&frame - m_frames.data()].buffer : VK_NULL_HANDLE,
            graph ? &*graph : nullptr);
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
    if (frame.submitted.pending) {
        frame.submission = ++m_submission;
    }
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkQueueSubmit2", result));
    }

    if (graph) {
        constexpr auto output = std::to_underlying(PresentationImage::Output);
        image.state = graph->finalStates[output];
        slot.graphStates = graph->finalStates;
        slot.graphStates[output] = {};
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
    if (m_lighting && m_lighting->environmentLighting && !m_environmentReady) {
        return std::unexpected(unavailable("environment lighting is waiting for its first upload"));
    }
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
    auto result = draw(frame, screenshot != nullptr || m_deferCapture);
    if (!result) {
        m_failed = true;
        return std::unexpected(std::move(result.error()));
    }
    m_rebuild = *result != DrawStatus::Presented;
    if (*result == DrawStatus::OutOfDate) {
        return PresentStatus::Retry;
    }
    m_swapchain.readbacks[m_frameIndex].captured = screenshot != nullptr || m_deferCapture;
    if (screenshot) {
        if (auto captured = readScreenshot(frame, *screenshot); !captured) {
            m_failed = true;
            return std::unexpected(std::move(captured.error()));
        }
    }
    m_frameIndex = (m_frameIndex + 1) % m_frames.size();
    return PresentStatus::Presented;
}

std::expected<void, core::Error>
Presentation::collectCapture(std::size_t index, FrameScreenshot& output) {
    if (index >= m_frames.size() || !m_swapchain.readbacks[index].captured) {
        return std::unexpected(
            unavailable("deferred capture is unavailable after swapchain rebuild"));
    }
    return readScreenshot(m_frames[index], output);
}

std::expected<void, core::Error>
Presentation::waitForBindingFrames(core::DiagnosticSession& diagnostics, std::size_t& waits) {
    for (auto& frame : m_frames) {
        if (!frame.submitted.pending) {
            continue;
        }
        const auto status = vkGetFenceStatus(m_device, frame.submitted.handle);
        if (status == VK_SUCCESS) {
            frame.submitted.pending = false;
            continue;
        }
        if (status != VK_NOT_READY) {
            return std::unexpected(failed("checking descriptor set users", status));
        }
        ++waits;
        const auto trace = [&](std::string_view phase) {
            if (!diagnostics.enabled(core::LogLevel::Debug)) {
                return;
            }
            core::Event event;
            event.level = core::LogLevel::Debug;
            event.module = "rhi";
            event.eventId = "rhi.binding.wait";
            event.message = "reason=descriptor_update phase=" + std::string(phase) +
                            " frame=" + std::to_string(&frame - m_frames.data()) +
                            " submission=" + std::to_string(frame.submission);
            (void)diagnostics.emit(event);
        };
        trace("begin");
        const auto result =
            vkWaitForFences(m_device, 1, &frame.submitted.handle, VK_TRUE, kFrameTimeout);
        if (result != VK_SUCCESS) {
            return std::unexpected(failed("waiting for descriptor set users", result));
        }
        trace("complete");
        frame.submitted.pending = false;
    }
    return {};
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

std::expected<void, core::Error> Presentation::publishStaticScene(
    std::vector<StaticSceneDraw::Draw> draws,
    std::shared_ptr<StaticSceneIndirect::Scene> indirect,
    const std::array<float, 16>& inverseProjection,
    const std::array<float, 12>& viewToWorld) {
    if (auto error = drainFrames()) {
        return std::unexpected(std::move(*error));
    }
    m_staticScene->publish(std::move(draws), std::move(indirect));
    m_inverseProjection = inverseProjection;
    m_viewToWorld = viewToWorld;
    return {};
}

std::expected<void, core::Error>
Presentation::publishEnvironment(const EnvironmentUpload& upload, bool verifyRollback) {
    auto candidate = m_staticScene->prepareEnvironment(upload, verifyRollback);
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    if (auto error = drainFrames()) {
        m_staticScene->destroyEnvironment(*candidate);
        return std::unexpected(std::move(*error));
    }
    std::swap(m_environment, *candidate);
    m_environmentReady = true;
    if (m_swapchain.handle) {
        writeEnvironmentDescriptors();
    }
    m_staticScene->destroyEnvironment(*candidate);
    return {};
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
    if (m_staticScene) {
        m_staticScene->destroyEnvironment(m_environment);
    }
    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(m_device, std::exchange(m_pipelineLayout, VK_NULL_HANDLE), nullptr);
    }
    if (m_lightingLayout) {
        vkDestroyPipelineLayout(m_device, std::exchange(m_lightingLayout, VK_NULL_HANDLE), nullptr);
    }
    if (m_lightingSetLayout) {
        vkDestroyDescriptorSetLayout(m_device, std::exchange(m_lightingSetLayout, VK_NULL_HANDLE), nullptr);
    }
    if (m_lightingSampler) {
        vkDestroySampler(m_device, std::exchange(m_lightingSampler, VK_NULL_HANDLE), nullptr);
    }
    if (m_environmentSampler) {
        vkDestroySampler(m_device, std::exchange(m_environmentSampler, VK_NULL_HANDLE), nullptr);
    }
    return error;
}

void Presentation::destroySwapchain() noexcept {
    if (m_swapchain.lightingPool) {
        vkDestroyDescriptorPool(m_device, m_swapchain.lightingPool, nullptr);
    }
    for (VkPipeline pipeline : {m_swapchain.directPipeline, m_swapchain.displayPipeline}) {
        if (pipeline) {
            vkDestroyPipeline(m_device, pipeline, nullptr);
        }
    }
    if (m_staticScene) {
        for (auto& slot : m_swapchain.attachments) {
            for (auto& attachment : slot.gbuffer) {
                m_staticScene->destroyAttachment(attachment);
            }
            m_staticScene->destroyAttachment(slot.hdr);
            m_staticScene->destroyAttachment(slot.depth);
        }
    }
    for (const Image& image : m_swapchain.images) {
        if (image.view) {
            vkDestroyImageView(m_device, image.view, nullptr);
        }
    }
    if (m_swapchain.pipeline) {
        vkDestroyPipeline(m_device, std::exchange(m_swapchain.pipeline, VK_NULL_HANDLE), nullptr);
    }
    for (const auto& readback : m_swapchain.readbacks) {
        if (readback.buffer) {
            vkDestroyBuffer(m_device, readback.buffer, nullptr);
        }
        if (readback.memory) {
            vkFreeMemory(m_device, readback.memory, nullptr);
        }
    }
    if (m_swapchain.handle) {
        vkDestroySwapchainKHR(m_device, m_swapchain.handle, nullptr);
    }
    m_swapchain = {};
}

} // namespace ar::rhi::detail
