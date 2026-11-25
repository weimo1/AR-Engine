#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <volk.h>

#include "OverlayDraw.h"
#include "core/Error.h"
#include "resources/StaticSceneDraw.h"
#include "rhi/FrameTimings.h"
#include "rhi/Overlay.h"
#include "rhi/PresentationGraph.h"
#include "rhi/PresentationSpec.h"

namespace ar::core {
class DiagnosticSession;
}

namespace ar::rhi {
struct TriangleShaders;
struct DeferredLighting;
struct FrameScreenshot;
enum class PresentStatus;
enum class PresentMode;
} // namespace ar::rhi

namespace ar::rhi::detail {

// Owns the swapchain and its frame resources. The parent closes it before releasing the device.
class DescriptorHeapTable;
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
        const PresentationSpec* spec = nullptr,
        DescriptorHeapTable* heapBinding = nullptr,
        DeviceResources* resources = nullptr,
        const OverlayShaders* overlayShaders = nullptr,
        PresentMode requestedPresentMode = {});
    std::expected<void, core::Error> publishStaticScene(
        std::vector<StaticSceneDraw::Draw> draws,
        std::shared_ptr<StaticSceneIndirect::Scene> indirect);
    void setFrameView(const FrameView& view) noexcept {
        m_frameView = view;
    }
    void setDirectionalLighting(const DirectionalLightingState& state) noexcept {
        m_lightingState = state;
    }
    void setExposureEV(float exposureEV) noexcept {
        m_spec.displayExposureEV = exposureEV;
    }
    void setDebugView(DebugViewMode mode) noexcept {
        m_debugView = mode;
    }
    [[nodiscard]] DebugViewMode debugView() const noexcept {
        return m_debugView;
    }
    std::expected<void, core::Error> uploadOverlayFont(const OverlayFontAtlas& atlas);
    std::expected<void, core::Error>
    publishEnvironment(const EnvironmentUpload& upload, bool verifyRollback);
    std::expected<PresentStatus, core::Error> presentFrame(
        VkExtent2D framebuffer,
        FrameScreenshot* screenshot,
        const OverlayDrawList* overlay = nullptr);
    [[nodiscard]] PresentMode activePresentMode() const noexcept {
        return m_activePresentMode;
    }
    [[nodiscard]] CpuFrameTimings lastCpuTimings() const noexcept {
        return m_lastCpuTimings;
    }
    [[nodiscard]] GpuFrameTimings latestGpuTimings() const noexcept {
        return m_latestTimings;
    }
    [[nodiscard]] std::vector<GpuFrameTimings> takeCompletedGpuTimings();
    std::expected<std::vector<GpuFrameTimings>, core::Error> drainCompletedGpuTimings();
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
    static constexpr std::uint32_t kTimestampCount = 14u;
    enum class TimestampSlot : std::uint32_t {
        FrameBegin = 0u,
        ShadowBegin = 1u,
        ShadowEnd = 2u,
        CullBegin = 3u,
        CullEnd = 4u,
        GeometryBegin = 5u,
        GeometryEnd = 6u,
        LightingBegin = 7u,
        LightingEnd = 8u,
        DisplayBegin = 9u,
        DisplayEnd = 10u,
        OverlayBegin = 11u,
        OverlayEnd = 12u,
        FrameEnd = 13u,
    };
    enum TimestampPassBits : std::uint32_t {
        kPassShadow = 1u << 0,
        kPassCull = 1u << 1,
        kPassGeometry = 1u << 2,
        kPassLighting = 1u << 3,
        kPassDisplay = 1u << 4,
        kPassOverlay = 1u << 5,
    };

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
    enum class PipelineKind { DirectGeometry, IndirectGeometry, DeferredLighting, Display };

    struct alignas(16) LightingFrameGpu {
        std::array<float, 16> inverseProjection{};
        std::array<float, 16> viewToWorld{};
        std::array<float, 16> lightViewProj{};
        std::array<float, 4> lightDirView{};
        std::array<float, 4> lightColor{};
        std::array<float, 4> shadowParams{};
    };

    [[nodiscard]] bool activeShadowPass() const noexcept {
        return m_lighting != nullptr && m_lighting->shadow.has_value() &&
               m_lightingState.shadowEnabled;
    }

    struct Fence {
        VkFence handle = VK_NULL_HANDLE;
        bool pending = false;
    };

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkQueryPool queryPool = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkSemaphore renderingFinished = VK_NULL_HANDLE;
        Fence acquired;
        Fence submitted;
        std::uint64_t submission = 0;
        bool hasPendingTimestamps = false;
        std::uint64_t timestampFrame = 0u;
        std::uint32_t timestampPassMask = 0u;
        Fence presented;
    };

    std::expected<void, core::Error> createFrame(Frame& frame, std::uint32_t graphicsFamily);
    std::expected<bool, core::Error> createSwapchain(VkExtent2D framebuffer);
    void destroySwapchain() noexcept;
    std::expected<void, core::Error> createGraphicsPipeline(
        VkFormat format,
        const std::vector<std::uint32_t>& vertexCode,
        const std::vector<std::uint32_t>& fragmentCode,
        VkPipeline& output,
        PipelineKind kind = PipelineKind::DirectGeometry);
    std::expected<void, core::Error> createLightingLayout();
    std::expected<void, core::Error> createPersistentLightingResources();
    std::expected<void, core::Error> createShadowPipeline(
        bool indirect,
        const std::vector<std::uint32_t>& vertexCode,
        VkPipelineLayout layout,
        VkPipeline& output);
    std::expected<void, core::Error> createLightingResources(VkImageUsageFlags usage);
    std::expected<void, core::Error> writeAttachmentHeapDescriptors();
    void resetAttachmentHeapDescriptors() noexcept;
    std::expected<void, core::Error>
    writeEnvironmentHeapDescriptors(const StaticSceneDraw::Environment& environment);
    void resetEnvironmentHeapDescriptors() noexcept;
    std::expected<void, core::Error> writeShadowHeapDescriptors();
    void resetShadowHeapDescriptors() noexcept;
    void writeEnvironmentDescriptors() const;
    std::expected<void, core::Error> writeLightingFrame(std::size_t frameIndex, bool activeShadows);
    void writeTimestamp(
        VkCommandBuffer commands,
        VkQueryPool pool,
        TimestampSlot slot,
        VkPipelineStageFlags2 stage) const noexcept;
    void harvestTimestamps(Frame& frame) noexcept;
    void recordShadow(
        VkCommandBuffer commands,
        VkQueryPool queryPool,
        const Image& output,
        const PresentationGraphPlan& plan) const;
    void recordLighting(
        VkCommandBuffer commands,
        VkQueryPool queryPool,
        const Image& output,
        const PresentationGraphPlan& plan,
        DebugViewMode effectiveView) const;
    std::expected<void, core::Error> createReadback(VkPhysicalDevice physical, Readback& readback);
    std::expected<void, core::Error> readScreenshot(Frame& frame, FrameScreenshot& screenshot);
    void copyRawProbes(VkCommandBuffer commands, VkBuffer destination) const;
    std::expected<DrawStatus, core::Error>
    draw(Frame& frame, bool capture, const OverlayDrawList* overlay);
    std::expected<void, core::Error> record(
        Frame& frame,
        const Image& image,
        VkBuffer capture,
        const PresentationGraphPlan* plan,
        const OverlayDrawList* overlay,
        bool overlayActive);
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
    VkSampler m_shadowSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_lightingSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_lightingLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_displaySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_displayLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_directShadowLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_indirectShadowLayout = VK_NULL_HANDLE;
    VkPipeline m_directShadowPipeline = VK_NULL_HANDLE;
    VkPipeline m_indirectShadowPipeline = VK_NULL_HANDLE;
    std::array<StaticSceneDraw::UniformBuffer, 2> m_lightingFrames{};
    std::array<StaticSceneDraw::Attachment, 2> m_shadowDepth{};
    std::array<PresentationImageState, 2> m_shadowStates{};
    FrameView m_frameView{};
    DirectionalLightingState m_lightingState{};
    DebugViewMode m_debugView = DebugViewMode::Final;
    StaticSceneDraw::Environment m_environment{};
    bool m_environmentReady = false;
    VkExtent2D m_requestedExtent{};
    SwapchainResources m_swapchain;
    VkDescriptorSet m_bindingSet = VK_NULL_HANDLE;
    DescriptorHeapTable* m_heapBinding = nullptr;
    StaticSceneDraw* m_staticScene = nullptr;
    std::optional<OverlayDraw> m_overlay;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::array<Frame, 2> m_frames{};
    std::uint64_t m_submission = 0;
    float m_timestampPeriodNs = 0.F;
    std::uint32_t m_timestampValidBits = 0u;
    std::string m_timestampUnavailableReason;
    PresentMode m_requestedPresentMode{};
    PresentMode m_activePresentMode{};
    CpuFrameTimings m_lastCpuTimings{};
    GpuFrameTimings m_latestTimings{};
    std::vector<GpuFrameTimings> m_completedTimings;
    std::array<DeviceResources*, 2> m_cullFrames{};
    std::uint32_t m_cullObjects = 0;
    std::size_t m_frameIndex = 0;
    bool m_failed = false;
    bool m_rebuild = false;
};

} // namespace ar::rhi::detail
