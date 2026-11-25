#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

#include <volk.h>

#include "core/Error.h"
#include "resources/DeviceResources.h"
#include "rhi/Overlay.h"

namespace ar::rhi::detail {

class DescriptorHeapTable;

// Renders lightweight 2D UI draw lists (Dear ImGui) over the swapchain output using the active
// binding path (Descriptor Set on 1.3, or Descriptor Heap on 1.4 with zero DescriptorPool/Set/
// PipelineLayout). Owned by Presentation; close() must run after draining all in-flight frames
// and before DeviceResources destroys its VMA allocator.
class OverlayDraw {
public:
    OverlayDraw(
        VkDevice device,
        DeviceResources& resources,
        DescriptorHeapTable* heapBinding,
        const OverlayShaders& shaders) noexcept;
    ~OverlayDraw();

    OverlayDraw(const OverlayDraw&) = delete;
    OverlayDraw& operator=(const OverlayDraw&) = delete;

    [[nodiscard]] std::expected<void, core::Error> open();
    [[nodiscard]] std::expected<void, core::Error> createPipeline(VkFormat colorFormat);
    void destroyPipeline() noexcept;

    // Caller must drain all in-flight frames before replacing an existing font atlas.
    [[nodiscard]] std::expected<void, core::Error> uploadFont(const OverlayFontAtlas& atlas);
    [[nodiscard]] bool hasFont() const noexcept {
        return m_fontReady;
    }

    // Writes per-frame dynamic vertex/index buffers after the slot's submission fence has signaled.
    [[nodiscard]] std::expected<bool, core::Error>
    prepareFrame(std::size_t frameIndex, const OverlayDrawList& list);

    void record(
        VkCommandBuffer commands,
        VkImageView targetView,
        VkExtent2D extent,
        bool swapchainIsSrgb,
        std::size_t frameIndex,
        const OverlayDrawList& list) const;

    void close() noexcept;

private:
    struct FrameBuffers {
        DeviceResources::BufferSlot vertices{};
        DeviceResources::BufferSlot indices{};
    };

    void releaseFont() noexcept;
    [[nodiscard]] std::expected<void, core::Error> ensureBuffer(
        DeviceResources::BufferSlot& slot, std::size_t requiredBytes, VkBufferUsageFlags usage);

    VkDevice m_device = VK_NULL_HANDLE;
    DeviceResources& m_resources;
    DescriptorHeapTable* m_heapBinding = nullptr;
    const OverlayShaders& m_shaders;

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    ImageHandle m_fontImageHandle{};
    ImageViewHandle m_fontViewHandle{};
    VkImage m_fontImage = VK_NULL_HANDLE;
    VkImageView m_fontView = VK_NULL_HANDLE;
    bool m_fontReady = false;

    std::array<FrameBuffers, 2> m_frames{};
};

} // namespace ar::rhi::detail
