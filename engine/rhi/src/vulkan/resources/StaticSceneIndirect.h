#pragma once

#include "DeviceResources.h"
#include "rhi/RhiSession.h"

namespace ar::rhi::detail {

class StaticSceneIndirect {
    using Buffer = DeviceResources::BufferSlot;

public:
    struct alignas(16) Instance {
        std::array<float, 16> clip;
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
        Buffer vertices, indices, instances, materials;
        struct Frame {
            Buffer commands, counts;
            VkDescriptorSet set = VK_NULL_HANDLE;
        };
        std::array<Frame, 2> frames;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        std::vector<std::uint64_t> objectIds;
        std::uint32_t capacity = 0;
    };

    explicit StaticSceneIndirect(DeviceResources& resources) : m_resources(resources) {}
    ~StaticSceneIndirect();
    StaticSceneIndirect(const StaticSceneIndirect&) = delete;
    std::expected<void, core::Error> open(const StaticIndirectShaders& shaders);
    std::expected<std::shared_ptr<Scene>, core::Error> prepare(
        const StaticIndirectGeometry& geometry,
        std::span<const Instance> instances,
        std::span<const Material> materials);
    VkDescriptorSetLayout layout() const noexcept {
        return m_setLayout;
    }
    void cull(
        const Scene& scene,
        VkCommandBuffer command,
        std::size_t slot,
        float aspect,
        const PresentationGraphPlan& graph) const;
    void draw(
        const Scene& scene,
        VkCommandBuffer command,
        std::size_t slot,
        float aspect,
        VkPipelineLayout layout,
        VkDescriptorSet textures) const;
    void barrier(
        const Scene& scene,
        VkCommandBuffer command,
        std::size_t slot,
        const PresentationGraphPlan& graph,
        IndirectPhase phase) const;
    std::expected<FrameScreenshot::IndirectVisibility, core::Error>
    visibility(const Scene& scene, std::size_t slot);
    void abandon() noexcept {
        m_abandoned = true;
    }

private:
    DeviceResources& m_resources;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_computeLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPhysicalDeviceLimits m_limits{};
    std::uint32_t m_capacity = 0;
    bool m_abandoned = false;
};

} // namespace ar::rhi::detail
