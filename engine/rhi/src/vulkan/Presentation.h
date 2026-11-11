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
struct FrameScreenshot;
} // namespace ar::rhi

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
        const TriangleShaders* shaders,
        bool enableScreenshot);
    std::expected<void, core::Error> presentFrame(FrameScreenshot* screenshot);
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
    std::expected<void, core::Error> createReadback(VkPhysicalDevice physical);
    std::expected<void, core::Error> readScreenshot(Frame& frame, FrameScreenshot& screenshot);
    std::expected<void, core::Error> draw(Frame& frame, bool capture);
    std::expected<void, core::Error>
    record(VkCommandBuffer commands, const Image& image, bool capture);
    VkResult drain(Frame& frame, std::uint64_t timeout);

    VkDevice m_device;
    VkQueue m_graphicsQueue;
    VkQueue m_presentQueue;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkBuffer m_readback = VK_NULL_HANDLE;
    VkDeviceMemory m_readbackMemory = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    std::size_t m_readbackBytes = 0;
    bool m_hostCoherent = false;
    VkExtent2D m_extent{};
    std::vector<Image> m_images;
    std::array<Frame, 2> m_frames{};
    std::size_t m_frameIndex = 0;
    bool m_failed = false;
};

} // namespace ar::rhi::detail
