#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include <volk.h>

#include "core/Error.h"
#include "rhi/PresentationGraph.h"
#include "rhi/PresentationSpec.h"
#include "resources/StaticSceneDraw.h"

namespace ar::core {
class DiagnosticSession;
}

namespace ar::rhi {
struct TriangleShaders;
struct DeferredLighting;
struct FrameScreenshot;
enum class PresentStatus;
} // namespace ar::rhi

namespace ar::rhi::detail {

// Owns the swapchain and its frame resources. The parent closes it before releasing the device.
class DeviceResources;
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
        bool enableScreenshot,
        VkDescriptorSetLayout bindingLayout,
        VkDescriptorSet bindingSet,
        StaticSceneDraw* staticScene = nullptr,
        bool enableGBuffer = false,
        const DeferredLighting* lighting = nullptr,
        PlanPresentationGraph planner = nullptr,
        const PresentationSpec* spec = nullptr);
    std::expected<void, core::Error> publishStaticScene(
        std::vector<StaticSceneDraw::Draw> draws,
        std::shared_ptr<StaticSceneIndirect::Scene> indirect);
    void setFrameView(const FrameView& view) noexcept {
        m_frameView = view;
    }
    std::expected<void, core::Error>
    publishEnvironment(const EnvironmentUpload& upload, bool verifyRollback);
    std::expected<PresentStatus, core::Error>
    presentFrame(VkExtent2D framebuffer, FrameScreenshot* screenshot);
    std::optional<core::Error> close();
    void setCullFrames(std::array<DeviceResources*, 2> frames, std::uint32_t objects) noexcept {
        m_cullFrames = frames;
        m_cullObjects = objects;
    }
    std::expected<void, core::Error>
    waitForBindingFrames(core::DiagnosticSession& diagnostics, std::size_t& waits);
    void deferCapture(bool enabled) noexcept {
        m_deferCapture = enabled;
    }
    std::expected<void, core::Error> collectCapture(std::size_t index, FrameScreenshot& output);
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
        PresentationImageState state{};
    };

    struct Readback {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        bool coherent = false;
        bool captured = false;
    };

    struct SwapchainResources {
        VkSwapchainKHR handle = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipeline directPipeline = VK_NULL_HANDLE;
        VkPipeline displayPipeline = VK_NULL_HANDLE;
        VkDescriptorPool lightingPool = VK_NULL_HANDLE;
        std::array<Readback, 2> readbacks{};
        struct SlotAttachments {
            StaticSceneDraw::Attachment depth;
            std::array<StaticSceneDraw::Attachment, 3> gbuffer;
            StaticSceneDraw::Attachment hdr;
            PresentationStates graphStates{};
            VkDescriptorSet directSet = VK_NULL_HANDLE;
            VkDescriptorSet displaySet = VK_NULL_HANDLE;
        };
        std::array<SlotAttachments, 2> attachments{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        std::vector<Image> images;
        std::size_t readbackBytes = 0;
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
        std::uint64_t submission = 0;
        Fence presented;
    };

    std::expected<void, core::Error> createFrame(Frame& frame, std::uint32_t graphicsFamily);
    std::expected<bool, core::Error> createSwapchain(VkExtent2D framebuffer);
    void destroySwapchain() noexcept;
    std::expected<void, core::Error>
    createGraphicsPipeline(VkFormat format, const std::vector<std::uint32_t>& vertexCode,
                           const std::vector<std::uint32_t>& fragmentCode, VkPipeline& output,
                           bool fullscreen = false);
    std::expected<void, core::Error> createLightingLayout();
    std::expected<void, core::Error> createLightingResources(VkImageUsageFlags usage);
    void writeEnvironmentDescriptors() const;
    void recordLighting(
        VkCommandBuffer commands, const Image& output, const PresentationGraphPlan& plan) const;
    std::expected<void, core::Error> createReadback(VkPhysicalDevice physical, Readback& readback);
    std::expected<void, core::Error> readScreenshot(Frame& frame, FrameScreenshot& screenshot);
    void copyRawProbes(VkCommandBuffer commands, VkBuffer destination) const;
    std::expected<DrawStatus, core::Error> draw(Frame& frame, bool capture);
    std::expected<void, core::Error> record(
        VkCommandBuffer commands,
        const Image& image,
        VkBuffer capture,
        const PresentationGraphPlan* plan);
    void recordGraphBarriers(
        VkCommandBuffer commands,
        const Image& output,
        const PresentationGraphPlan& plan,
        PresentationPass pass) const;
    bool validGraphPlan(const PresentationGraphPlan& plan, bool capture) const;
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
    bool m_deferCapture = false;
    bool m_enableGBuffer = false;
    PresentationSpec m_spec{};
    PlanPresentationGraph m_planPresentation = nullptr;
    const DeferredLighting* m_lighting = nullptr;
    VkSampler m_lightingSampler = VK_NULL_HANDLE;
    VkSampler m_environmentSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_lightingSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_lightingLayout = VK_NULL_HANDLE;
    FrameView m_frameView{};
    StaticSceneDraw::Environment m_environment{};
    bool m_environmentReady = false;
    VkExtent2D m_requestedExtent{};
    SwapchainResources m_swapchain;
    VkDescriptorSet m_bindingSet = VK_NULL_HANDLE;
    StaticSceneDraw* m_staticScene = nullptr;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::array<Frame, 2> m_frames{};
    std::uint64_t m_submission = 0;
    std::array<DeviceResources*, 2> m_cullFrames{};
    std::uint32_t m_cullObjects = 0;
    std::size_t m_frameIndex = 0;
    bool m_failed = false;
    bool m_rebuild = false;
};

} // namespace ar::rhi::detail
