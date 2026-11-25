#pragma once

#include "DeviceResources.h"
#include "rhi/RhiSession.h"

namespace ar::rhi::detail {

class DescriptorHeapTable;

class StaticSceneIndirect {
    using Buffer = DeviceResources::BufferSlot;

public:
    struct alignas(16) FrameViewGpu {
        std::array<float, 16> viewProj;
        std::array<float, 16> view;
    };
    struct alignas(16) Instance {
        std::array<float, 16> model;
        std::array<float, 12> normal;
        std::array<float, 4> minimum;
        std::array<float, 4> maximum;
        std::uint32_t firstIndex, indexCount;
        std::int32_t vertexOffset;
        std::uint32_t material;
    };
    struct alignas(16) Material {
        std::array<float, 4> baseColor;
        float metallic, roughness;
        std::uint32_t albedo, metalRough;
    };
    struct Scene {
        explicit Scene(StaticSceneIndirect& owner) : owner(owner) {}
        ~Scene();
        Scene(const Scene&) = delete;
        StaticSceneIndirect& owner;
        Buffer vertices, indices, instances, materials, shadowCommands;
        VkDeviceAddress instancesAddress = 0;
        VkDeviceAddress materialsAddress = 0;
        struct Frame {
            Buffer commands, counts, view;
            VkDeviceAddress commandsAddress = 0;
            VkDeviceAddress countsAddress = 0;
            VkDeviceAddress viewAddress = 0;
            VkDescriptorSet set = VK_NULL_HANDLE;
        };
        std::array<Frame, 2> frames;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        std::vector<std::uint64_t> objectIds;
        std::vector<CpuCullInstance> cpuCullInstances;
        std::uint32_t capacity = 0;
        std::uint32_t shadowInstanceCount = 0;
    };

    struct CpuCullTiming {
        std::uint32_t visibleCount = 0;
        std::uint32_t drawCount = 0;
        std::uint32_t capacity = 0;
        double cullMs = 0.0;
    };

    struct IndirectCountSample {
        std::uint32_t visibleCount = 0;
        std::uint32_t drawCount = 0;
        std::uint32_t capacity = 0;
    };

    explicit StaticSceneIndirect(
        DeviceResources& resources, DescriptorHeapTable* heapBindings = nullptr)
        : m_resources(resources), m_heapBindings(heapBindings) {}
    ~StaticSceneIndirect();
    StaticSceneIndirect(const StaticSceneIndirect&) = delete;
    std::expected<void, core::Error> open(const StaticIndirectShaders& shaders);
    [[nodiscard]] StaticCullMode cullMode() const noexcept {
        return m_cullMode;
    }
    [[nodiscard]] bool readIndirectCountsEnabled() const noexcept {
        return m_readIndirectCounts;
    }
    std::expected<std::shared_ptr<Scene>, core::Error> prepare(
        const StaticIndirectGeometry& geometry,
        std::span<const Instance> instances,
        std::span<const Material> materials);
    std::expected<void, core::Error> publishHeapBuffers(const Scene& scene);
    std::expected<void, core::Error> updateFrameView(
        const Scene& scene, std::size_t slot, const FrameView& view, float inverseAspect);
    std::expected<CpuCullTiming, core::Error>
    cullCpu(const Scene& scene, std::size_t slot, const FrameView& view, float inverseAspect);
    VkDescriptorSetLayout layout() const noexcept {
        return m_setLayout;
    }
    void cull(
        const Scene& scene,
        VkCommandBuffer command,
        std::size_t slot,
        const PresentationGraphPlan& graph) const;
    void draw(
        const Scene& scene,
        VkCommandBuffer command,
        std::size_t slot,
        VkPipelineLayout layout,
        VkDescriptorSet textures) const;
    void drawShadow(
        const Scene& scene,
        VkCommandBuffer command,
        std::size_t slot,
        VkPipelineLayout layout,
        const std::array<float, 16>& lightViewProj) const;
    void barrier(
        const Scene& scene,
        VkCommandBuffer command,
        std::size_t slot,
        const PresentationGraphPlan& graph,
        IndirectPhase phase) const;
    std::expected<FrameScreenshot::IndirectVisibility, core::Error>
    visibility(const Scene& scene, std::size_t slot);
    std::expected<IndirectCountSample, core::Error>
    readCounts(const Scene& scene, std::size_t slot);
    void abandon() noexcept {
        m_abandoned = true;
    }

private:
    DeviceResources& m_resources;
    DescriptorHeapTable* m_heapBindings = nullptr;
    PFN_vkGetBufferDeviceAddress m_getBufferDeviceAddress = nullptr;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_computeLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPhysicalDeviceLimits m_limits{};
    std::uint32_t m_capacity = 0;
    StaticCullMode m_cullMode = StaticCullMode::GpuCompute;
    CpuFrustumCullFn m_cpuCull = nullptr;
    bool m_readIndirectCounts = false;
    std::vector<IndexedIndirectDrawCommand> m_cpuScratch;
    bool m_abandoned = false;
};

} // namespace ar::rhi::detail
