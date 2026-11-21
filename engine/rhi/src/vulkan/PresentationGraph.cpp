#include "Presentation.h"

#include <utility>

namespace ar::rhi::detail {
namespace {

VkImageLayout vkLayout(GraphLayout layout) {
    switch (layout) {
    case GraphLayout::Undefined:
        return VK_IMAGE_LAYOUT_UNDEFINED;
    case GraphLayout::ColorAttachment:
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case GraphLayout::DepthAttachment:
        return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    case GraphLayout::ShaderRead:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case GraphLayout::CopySource:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case GraphLayout::Present:
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

struct Scope {
    VkPipelineStageFlags2 stages = 0;
    VkAccessFlags2 access = 0;
};

Scope scope(GraphUsageMask mask) {
    Scope result{};
    const auto add = [&](GraphUsage use, VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
        if ((mask & std::to_underlying(use)) != 0) {
            result.stages |= stage;
            result.access |= access;
        }
    };
    add(GraphUsage::ColorWrite,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    add(GraphUsage::DepthWrite,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    add(GraphUsage::FragmentSample,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    add(GraphUsage::ComputeSample,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    add(GraphUsage::CopyRead, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    return result;
}

} // namespace

void Presentation::recordGraphBarriers(
    VkCommandBuffer commands,
    const Image& output,
    const PresentationGraphPlan& plan,
    PresentationPass pass) const {
    const auto& slot = m_swapchain.attachments[m_frameIndex];
    const std::array images{
        output.handle,
        slot.gbuffer[0].image,
        slot.gbuffer[1].image,
        slot.gbuffer[2].image,
        slot.depth.image,
        slot.hdr.image};
    std::array<VkImageMemoryBarrier2, images.size()> barriers{};
    std::uint32_t count = 0;
    for (const auto& access : plan.barriers[std::to_underlying(pass)]) {
        const auto source = scope(access.source);
        const auto destination = scope(access.destination);
        auto& barrier = barriers[count++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = source.stages;
        if (access.image == PresentationImage::Output &&
            access.after == GraphLayout::ColorAttachment) {
            // The semaphore wait at color output also orders the initial image transition.
            barrier.srcStageMask |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        }
        barrier.srcAccessMask = source.access;
        barrier.dstStageMask = destination.stages;
        barrier.dstAccessMask = destination.access;
        barrier.oldLayout = access.discard ? VK_IMAGE_LAYOUT_UNDEFINED : vkLayout(access.before);
        barrier.newLayout = vkLayout(access.after);
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[std::to_underlying(access.image)];
        const VkImageAspectFlags aspect = access.image == PresentationImage::Depth
                                              ? VK_IMAGE_ASPECT_DEPTH_BIT
                                              : VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    }
    if (count != 0) {
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = count;
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(commands, &dependency);
    }
}

bool Presentation::validGraphPlan(const PresentationGraphPlan& plan) const {
    const auto& slot = m_swapchain.attachments[m_frameIndex];
    const std::array bound{
        true,
        slot.gbuffer[0].image != VK_NULL_HANDLE,
        slot.gbuffer[1].image != VK_NULL_HANDLE,
        slot.gbuffer[2].image != VK_NULL_HANDLE,
        slot.depth.image != VK_NULL_HANDLE,
        slot.hdr.image != VK_NULL_HANDLE};
    for (const auto& pass : plan.barriers) {
        std::array<bool, bound.size()> seen{};
        for (const auto& barrier : pass) {
            const auto image = std::to_underlying(barrier.image);
            if (image >= bound.size() || !bound[image] || seen[image]) {
                return false;
            }
            seen[image] = true;
        }
    }
    return true;
}

} // namespace ar::rhi::detail
