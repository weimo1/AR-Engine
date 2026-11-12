#include "DeviceResources.h"

#include <limits>
#include <string>
#include <utility>

#include "VulkanResult.h"

namespace ar::rhi::detail {
namespace {

std::uint64_t nextTransfer = 1;
constexpr std::uint64_t kWaitNanoseconds = 5'000'000'000;

core::Error transferError(const char* operation, VkResult result) {
    return {
        core::ErrorCategory::InitializationFailed,
        core::ErrorStage::Capability,
        std::string(operation) + " returned " + vkResultName(result),
        core::ErrorOrigin{"vulkan", result}};
}

} // namespace

std::expected<void, core::Error> DeviceResources::ensureTransferCommands() {
    if (!m_transferPool) {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.queueFamilyIndex = m_queueFamily;
        VkCommandPool pool = VK_NULL_HANDLE;
        if (const auto result = vkCreateCommandPool(m_device, &info, nullptr, &pool);
            result != VK_SUCCESS) {
            return std::unexpected(transferError("vkCreateCommandPool", result));
        }
        m_transferPool = pool;
    }
    if (!m_transferCommands) {
        VkCommandBufferAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = m_transferPool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        if (const auto result = vkAllocateCommandBuffers(m_device, &info, &commands);
            result != VK_SUCCESS) {
            return std::unexpected(transferError("vkAllocateCommandBuffers", result));
        }
        m_transferCommands = commands;
    }
    if (!m_transferFence) {
        VkFenceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (const auto result = vkCreateFence(m_device, &info, nullptr, &fence);
            result != VK_SUCCESS) {
            return std::unexpected(transferError("vkCreateFence", result));
        }
        m_transferFence = fence;
    }
    return {};
}

std::expected<bool, core::Error> DeviceResources::settleTransfer(bool wait) {
    if (m_transferLost) {
        return std::unexpected(transferError("transfer fence", VK_ERROR_DEVICE_LOST));
    }
    if (!m_pendingTransfer) {
        return true;
    }
    const VkResult result =
        wait ? vkWaitForFences(m_device, 1, &m_transferFence, VK_TRUE, kWaitNanoseconds)
             : vkGetFenceStatus(m_device, m_transferFence);
    if (result == VK_NOT_READY) {
        return false;
    }
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) {
        m_pendingTransfer = false;
        m_transferLost = result == VK_ERROR_DEVICE_LOST;
    }
    if (result != VK_SUCCESS) {
        return std::unexpected(transferError("transfer fence", result));
    }
    m_completedTransfer = m_lastTransfer;
    return true;
}

std::expected<bool, core::Error> DeviceResources::pollTransfer(TransferToken token) {
    if (!token.value || token.value < m_firstTransfer || token.value > m_lastTransfer) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Capability,
                "transfer token does not belong to this session",
                std::nullopt});
    }
    if (token.value <= m_completedTransfer) {
        return true;
    }
    return settleTransfer(false);
}

std::expected<void, core::Error> DeviceResources::waitTransfer(TransferToken token) {
    auto ready = pollTransfer(token);
    if (!ready) {
        return std::unexpected(std::move(ready.error()));
    }
    if (!*ready) {
        if (auto settled = settleTransfer(true); !settled) {
            return std::unexpected(std::move(settled.error()));
        }
    }
    return {};
}

std::expected<TransferToken, core::Error> DeviceResources::transfer(
    VkBuffer buffer,
    ImageSlot* image,
    std::span<const std::byte> input,
    std::span<std::byte> output,
    bool hostVisibleDestination) {
    if (auto settled = settleTransfer(true); !settled) {
        return std::unexpected(std::move(settled.error()));
    }
    if (nextTransfer == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "transfer tokens exhausted",
                std::nullopt});
    }
    freeStaging();
    if (auto commands = ensureTransferCommands(); !commands) {
        return std::unexpected(std::move(commands.error()));
    }
    const bool upload = !input.empty();
    const auto bytes = upload ? input.size() : output.size();
    if (bytes > bufferSizeLimit()) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Capability,
                "staging size exceeds the buffer limit",
                std::nullopt});
    }
    auto staging = allocateBuffer({bytes, BufferMemory::HostVisible});
    if (!staging) {
        return std::unexpected(std::move(staging.error()));
    }
    m_staging = *staging;
    const VkBuffer stagingBuffer = m_staging.buffer;
    if (upload) {
        if (auto written = writeAllocation(m_staging.allocation, 0, input); !written) {
            return std::unexpected(std::move(written.error()));
        }
    }
    if (const auto result = vkResetCommandPool(m_device, m_transferPool, 0); result != VK_SUCCESS) {
        return std::unexpected(transferError("vkResetCommandPool", result));
    }
    if (const auto result = vkResetFences(m_device, 1, &m_transferFence); result != VK_SUCCESS) {
        return std::unexpected(transferError("vkResetFences", result));
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (const auto result = vkBeginCommandBuffer(m_transferCommands, &begin);
        result != VK_SUCCESS) {
        return std::unexpected(transferError("vkBeginCommandBuffer", result));
    }

    constexpr auto access = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    VkMemoryBarrier2 memory{};
    memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    memory.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    memory.srcAccessMask = access;
    memory.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    memory.dstAccessMask = access;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    const VkImageLayout layout =
        upload ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (image) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        if (image->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = access;
        }
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask =
            upload ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = image->layout;
        barrier.newLayout = layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image->image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(m_transferCommands, &dependency);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {image->desc.width, image->desc.height, 1};
        if (upload) {
            vkCmdCopyBufferToImage(
                m_transferCommands, stagingBuffer, image->image, layout, 1, &region);
        } else {
            vkCmdCopyImageToBuffer(
                m_transferCommands, image->image, layout, stagingBuffer, 1, &region);
        }
        dependency.imageMemoryBarrierCount = 0;
        dependency.pImageMemoryBarriers = nullptr;
    } else {
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(m_transferCommands, &dependency);
        const VkBufferCopy region{0, 0, bytes};
        vkCmdCopyBuffer(
            m_transferCommands,
            upload ? stagingBuffer : buffer,
            upload ? buffer : stagingBuffer,
            1,
            &region);
    }
    if (!upload || hostVisibleDestination) {
        memory.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        memory.dstAccessMask = hostVisibleDestination
                                   ? VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT
                                   : VK_ACCESS_2_HOST_READ_BIT;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(m_transferCommands, &dependency);
    }
    if (const auto result = vkEndCommandBuffer(m_transferCommands); result != VK_SUCCESS) {
        return std::unexpected(transferError("vkEndCommandBuffer", result));
    }
    VkCommandBufferSubmitInfo commands{};
    commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commands.commandBuffer = m_transferCommands;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commands;
    const VkResult result = vkQueueSubmit2(m_queue, 1, &submit, m_transferFence);
    m_pendingTransfer = result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        return std::unexpected(transferError("vkQueueSubmit2", result));
    }
    m_lastTransfer = nextTransfer++;
    if (!m_firstTransfer) {
        m_firstTransfer = m_lastTransfer;
    }
    if (image) {
        image->layout = layout;
    }
    if (!upload) {
        if (auto settled = settleTransfer(true); !settled) {
            return std::unexpected(std::move(settled.error()));
        }
        if (auto read = readAllocation(m_staging.allocation, 0, output); !read) {
            return std::unexpected(std::move(read.error()));
        }
    }
    return TransferToken{m_lastTransfer};
}

} // namespace ar::rhi::detail
