#pragma once

#include "DeviceResources.h"
#include "StaticSceneIndirect.h"
#include "rhi/EnvironmentUpload.h"

namespace ar::rhi::detail {

class DescriptorHeapTable;
class SetBindingTable;

class StaticSceneDraw {
    struct Mesh;

public:
    explicit StaticSceneDraw(
        DeviceResources& resources,
        bool enableGBuffer,
        SetBindingTable* bindings,
        DescriptorHeapTable* heapBindings = nullptr);
    ~StaticSceneDraw();
    StaticSceneDraw(const StaticSceneDraw&) = delete;

    struct Attachment {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct UniformBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkDeviceAddress address = 0;
    };
    struct Draw {
        std::shared_ptr<Mesh> mesh;
        StaticInstanceDraw instance;
    };

    std::expected<void, core::Error> open(const StaticIndirectShaders* indirect = nullptr);
    VkDescriptorSetLayout layout() const noexcept;
    VkDescriptorSetLayout indirectLayout() const noexcept;
    bool indirectEnabled() const noexcept {
        return m_indirect.has_value();
    }
    bool gpuCullEnabled() const noexcept {
        return m_indirect.has_value() && m_indirect->cullMode() == StaticCullMode::GpuCompute;
    }
    bool cpuCullEnabled() const noexcept {
        return m_indirect.has_value() && m_indirect->cullMode() == StaticCullMode::CpuFrustum;
    }
    bool readIndirectCountsEnabled() const noexcept {
        return m_indirect.has_value() && m_indirect->readIndirectCountsEnabled();
    }
    IndirectProducer indirectProducer() const noexcept {
        if (!m_indirect) {
            return IndirectProducer::None;
        }
        return m_indirect->cullMode() == StaticCullMode::CpuFrustum ? IndirectProducer::Cpu
                                                                    : IndirectProducer::GpuCompute;
    }
    std::expected<Attachment, core::Error> createAttachment(
        VkExtent2D extent,
        VkFormat format,
        VkImageAspectFlags aspect,
        VkImageUsageFlags usage,
        std::uint32_t mipCount = 1,
        bool cube = false);
    void destroyAttachment(Attachment& attachment) noexcept;
    std::expected<UniformBuffer, core::Error> createUniformBuffer(std::size_t bytes);
    std::expected<void, core::Error>
    writeUniformBuffer(const UniformBuffer& buffer, std::span<const std::byte> data);
    void destroyUniformBuffer(UniformBuffer& buffer) noexcept;

    using Environment = std::array<Attachment, 3>;
    std::expected<Environment, core::Error>
    prepareEnvironment(const EnvironmentUpload& upload, bool verifyRollback);
    void destroyEnvironment(Environment& environment) noexcept;
    std::expected<std::vector<Draw>, core::Error> prepare(
        std::span<const StaticMeshUpload> meshes,
        std::span<const StaticInstanceDraw> draws,
        bool verifyRollback);
    std::expected<std::shared_ptr<StaticSceneIndirect::Scene>, core::Error>
    prepareIndirect(std::span<const Draw> draws, const StaticIndirectGeometry* geometry);
    std::expected<void, core::Error>
    publish(std::vector<Draw> draws, std::shared_ptr<StaticSceneIndirect::Scene> indirect);
    std::expected<void, core::Error>
    updateFrameView(std::size_t frame, const FrameView& view, float inverseAspect);
    void record(
        VkCommandBuffer commands,
        VkPipelineLayout layout,
        const FrameView& view,
        float inverseAspect,
        std::size_t frame) const;
    void recordShadow(
        VkCommandBuffer commands,
        VkPipelineLayout layout,
        const std::array<float, 16>& lightViewProj,
        std::size_t frame) const;
    void
    cull(VkCommandBuffer commands, std::size_t frame, const PresentationGraphPlan& graph) const;
    std::expected<StaticSceneIndirect::CpuCullTiming, core::Error>
    cullCpu(std::size_t frame, const FrameView& view, float inverseAspect);
    void indirectReadback(
        VkCommandBuffer commands, std::size_t frame, const PresentationGraphPlan& graph) const;
    std::expected<void, core::Error> readVisibility(std::size_t frame, FrameScreenshot& data);
    std::expected<StaticSceneIndirect::IndirectCountSample, core::Error>
    readIndirectCounts(std::size_t frame);
    void abandon() noexcept;

private:
    std::expected<std::shared_ptr<Mesh>, core::Error> upload(const StaticMeshUpload& input);

    DeviceResources& m_resources;
    SetBindingTable* m_bindings;
    DescriptorHeapTable* m_heapBindings = nullptr;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    std::vector<Draw> m_active;
    std::optional<StaticSceneIndirect> m_indirect;
    std::shared_ptr<StaticSceneIndirect::Scene> m_indirectScene;
    // A candidate whose transfer fence could not be settled is held until retry or close.
    Environment m_pendingEnvironment{};
    bool m_abandoned = false;
    bool m_enableGBuffer = false;
};

} // namespace ar::rhi::detail
