#include "Presentation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "PresentationUtil.h"
#include "rhi/RhiSession.h"

namespace ar::rhi::detail {
namespace {

using presentation::create;
using presentation::failed;
using presentation::kFrameTimeout;
using presentation::probeStart;
using presentation::unavailable;

std::vector<std::array<std::uint32_t, 2>>
resolveProbePixels(VkExtent2D extent, const std::vector<std::array<float, 2>>& coords) {
    const auto clampAxis = [](std::uint32_t size, float fraction) -> std::uint32_t {
        if (size == 0) {
            return 0;
        }
        const auto raw = static_cast<std::int64_t>(static_cast<float>(size) * fraction);
        return static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(raw, 0, static_cast<std::int64_t>(size) - 1));
    };
    std::vector<std::array<std::uint32_t, 2>> pixels;
    pixels.reserve(coords.size());
    for (const auto& coord : coords) {
        pixels.push_back({clampAxis(extent.width, coord[0]), clampAxis(extent.height, coord[1])});
    }
    return pixels;
}

} // namespace

std::expected<void, core::Error>
Presentation::createReadback(VkPhysicalDevice physical, Readback& readback) {
    constexpr VkDeviceSize maxBytes = 128u * 1024u * 1024u;
    const auto pixels =
        static_cast<VkDeviceSize>(m_swapchain.extent.width) * m_swapchain.extent.height;
    if (pixels > maxBytes / 4) {
        return std::unexpected(unavailable("screenshot exceeds the 128 MiB readback limit"));
    }
    m_swapchain.readbackBytes = static_cast<std::size_t>(pixels * 4);
    VkDeviceSize totalBytes = m_swapchain.readbackBytes;
    if (m_enableGBuffer && !m_spec.probes.normalizedCoords.empty()) {
        const VkDeviceSize probeBytes = static_cast<VkDeviceSize>(m_spec.probes.sampleStrideBytes) *
                                        m_spec.probes.normalizedCoords.size();
        const VkDeviceSize start = probeStart(m_swapchain.readbackBytes);
        if (probeBytes > maxBytes || start > maxBytes - probeBytes) {
            return std::unexpected(unavailable("probe capture exceeds the 128 MiB readback limit"));
        }
        totalBytes = start + probeBytes;
    }
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = totalBytes;
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
        if (m_enableGBuffer && !m_spec.probes.normalizedCoords.empty()) {
            auto& raw = captured.rawProbes.emplace();
            raw.sampleStrideBytes = m_spec.probes.sampleStrideBytes;
            raw.attachmentByteOffsets = m_spec.probes.attachmentByteOffsets;
            raw.pixels = resolveProbePixels(m_swapchain.extent, m_spec.probes.normalizedCoords);
            raw.includesHdr = m_lighting != nullptr;
            const std::size_t totalProbeBytes =
                static_cast<std::size_t>(raw.sampleStrideBytes) * raw.pixels.size();
            raw.bytes.resize(totalProbeBytes);
            const auto* probeSrc =
                static_cast<const std::byte*>(mapped) + probeStart(m_swapchain.readbackBytes);
            std::memcpy(raw.bytes.data(), probeSrc, totalProbeBytes);
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

void Presentation::copyRawProbes(VkCommandBuffer commands, VkBuffer destination) const {
    if (m_spec.probes.normalizedCoords.empty()) {
        return;
    }
    const auto& slot = m_swapchain.attachments[m_frameIndex];
    const std::array images{
        slot.gbuffer[0].image,
        slot.gbuffer[1].image,
        slot.gbuffer[2].image,
        slot.depth.image,
        slot.hdr.image};
    const std::uint32_t count = m_lighting ? 5u : 4u;
    const auto pixels = resolveProbePixels(m_swapchain.extent, m_spec.probes.normalizedCoords);
    std::vector<VkBufferImageCopy> copies(pixels.size());
    const VkDeviceSize baseOffset = probeStart(m_swapchain.readbackBytes);
    for (std::uint32_t i = 0; i < count; ++i) {
        const VkImageAspectFlags aspect =
            i == 3u ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        for (std::size_t j = 0; j < pixels.size(); ++j) {
            auto& copy = copies[j];
            copy = {};
            copy.bufferOffset = baseOffset +
                                static_cast<VkDeviceSize>(j) * m_spec.probes.sampleStrideBytes +
                                m_spec.probes.attachmentByteOffsets[i];
            copy.imageSubresource = {aspect, 0, 0, 1};
            copy.imageOffset = {
                static_cast<std::int32_t>(pixels[j][0]),
                static_cast<std::int32_t>(pixels[j][1]),
                0};
            copy.imageExtent = {1, 1, 1};
        }
        vkCmdCopyImageToBuffer(
            commands,
            images[i],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            destination,
            static_cast<std::uint32_t>(copies.size()),
            copies.data());
    }
}

std::expected<void, core::Error>
Presentation::collectCapture(std::size_t index, FrameScreenshot& output) {
    if (index >= m_frames.size() || !m_swapchain.readbacks[index].captured) {
        return std::unexpected(
            unavailable("deferred capture is unavailable after swapchain rebuild"));
    }
    return readScreenshot(m_frames[index], output);
}

} // namespace ar::rhi::detail
