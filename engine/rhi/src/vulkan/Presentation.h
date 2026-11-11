#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include <volk.h>

#include "core/Error.h"

namespace ar::rhi {
struct TriangleShaders;
}

namespace ar::rhi::detail {

// Owns the swapchain and its frame resources. The parent closes it before releasing the device.
class Presentation {
public:
    Presentation(VkDevice device, VkQueue graphicsQueue, VkQueue presentQueue) noexcept;

    Presentation(const Presentation&) = delete;
    Presentation& operator=(const Presentation&) = delete;

    std::expected<void, core::Error> open(
        VkPhysicalDevice physical,
        VkSurfaceKHR surface,
        std::uint32_t graphicsFamily,
        std::uint32_t presentFamily,
        VkExtent2D framebuffer,
        const TriangleShaders* shaders);
    std::expected<void, core::Error> presentFrame();
    std::optional<core::Error> close();
    [[nodiscard]] bool hasSwapchain() const noexcept {
        return m_swapchain != VK_NULL_HANDLE;
    }
    [[nodiscard]] std::size_t imageCount() const noexcept {
        return m_images.size();
    }

private:
    struct Image {
        VkImage handle = VK_NULL_HANDLE; // Owned by the swapchain.
        VkImageView view = VK_NULL_HANDLE;
    };

    struct Fence {
        VkFence handle = VK_NULL_HANDLE;
        bool pending = false;
    };

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkSemaphore renderingFinished = VK_NULL_HANDLE;
        Fence acquired;
        Fence submitted;
        Fence presented;
    };

    std::expected<void, core::Error> createFrame(Frame& frame, std::uint32_t graphicsFamily);
    std::expected<void, core::Error>
    createTrianglePipeline(VkFormat format, const TriangleShaders& shaders);
    std::expected<void, core::Error> draw(Frame& frame);
    std::expected<void, core::Error> record(VkCommandBuffer commands, const Image& image);
    VkResult drain(Frame& frame, std::uint64_t timeout);

    VkDevice m_device;
    VkQueue m_graphicsQueue;
    VkQueue m_presentQueue;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkExtent2D m_extent{};
    std::vector<Image> m_images;
    std::array<Frame, 2> m_frames{};
    std::size_t m_frameIndex = 0;
    bool m_failed = false;
};

} // namespace ar::rhi::detail
