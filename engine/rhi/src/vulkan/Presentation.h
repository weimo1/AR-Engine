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
enum class PresentStatus;
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
    std::expected<PresentStatus, core::Error>
    presentFrame(VkExtent2D framebuffer, FrameScreenshot* screenshot);
    std::optional<core::Error> close();
    [[nodiscard]] bool hasSwapchain() const noexcept {
        return m_swapchain.handle != VK_NULL_HANDLE;
    }
    [[nodiscard]] std::size_t imageCount() const noexcept {
        return m_swapchain.images.size();
    }

private:
    struct Image {
        VkImage handle = VK_NULL_HANDLE; // Owned by the swapchain.
        VkImageView view = VK_NULL_HANDLE;
    };

    struct SwapchainResources {
        VkSwapchainKHR handle = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkBuffer readback = VK_NULL_HANDLE;
        VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        std::vector<Image> images;
        std::size_t readbackBytes = 0;
        bool hostCoherent = false;
    };

    enum class DrawStatus { Presented, Suboptimal, OutOfDate };

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
    std::expected<bool, core::Error> createSwapchain(VkExtent2D framebuffer);
    void destroySwapchain() noexcept;
    std::expected<void, core::Error>
    createTrianglePipeline(VkFormat format, const TriangleShaders& shaders);
    std::expected<void, core::Error> createReadback(VkPhysicalDevice physical);
    std::expected<void, core::Error> readScreenshot(Frame& frame, FrameScreenshot& screenshot);
    std::expected<DrawStatus, core::Error> draw(Frame& frame, bool capture);
    std::expected<void, core::Error>
    record(VkCommandBuffer commands, const Image& image, bool capture);
    VkResult drain(Frame& frame, std::uint64_t timeout);
    std::optional<core::Error> drainFrames();

    VkDevice m_device;
    VkQueue m_graphicsQueue;
    VkQueue m_presentQueue;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    std::uint32_t m_graphicsFamily = 0;
    std::uint32_t m_presentFamily = 0;
    // The RHI session owns the bytecode for the full lifetime of Presentation.
    const TriangleShaders* m_shaders = nullptr;
    bool m_enableScreenshot = false;
    VkExtent2D m_requestedExtent{};
    SwapchainResources m_swapchain;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::array<Frame, 2> m_frames{};
    std::size_t m_frameIndex = 0;
    bool m_failed = false;
    bool m_rebuild = false;
};

} // namespace ar::rhi::detail
