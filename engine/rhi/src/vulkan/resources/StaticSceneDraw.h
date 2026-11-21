#pragma once

#include "DeviceResources.h"
#include "StaticSceneIndirect.h"
#include "rhi/EnvironmentUpload.h"

namespace ar::rhi::detail {

class SetBindingTable;

class StaticSceneDraw {
    struct Mesh;

public:
    explicit StaticSceneDraw(
        DeviceResources& resources, bool enableGBuffer, SetBindingTable* bindings);
    ~StaticSceneDraw();
    StaticSceneDraw(const StaticSceneDraw&) = delete;

    static constexpr VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    struct Attachment {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
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
    std::expected<Attachment, core::Error> createAttachment(
        VkExtent2D extent,
        VkFormat format,
        VkImageAspectFlags aspect,
        VkImageUsageFlags usage,
        std::uint32_t mipCount = 1,
        bool cube = false);
    void destroyAttachment(Attachment& attachment) noexcept;

    using Environment = std::array<Attachment, 3>;
    std::expected<Environment, core::Error>
    prepareEnvironment(const EnvironmentUpload& upload, bool verifyRollback);
    void destroyEnvironment(Environment& environment) noexcept;
    std::expected<std::vector<Draw>, core::Error> prepare(
        std::span<const StaticMeshUpload> meshes, std::span<const StaticInstanceDraw> draws,
        bool verifyRollback);
    std::expected<std::shared_ptr<StaticSceneIndirect::Scene>, core::Error>
    prepareIndirect(std::span<const Draw> draws, const StaticIndirectGeometry* geometry);
    void
    publish(std::vector<Draw> draws, std::shared_ptr<StaticSceneIndirect::Scene> indirect) noexcept;
    void record(
        VkCommandBuffer commands,
        VkPipelineLayout layout,
        float inverseAspect,
        std::size_t frame) const;
    void cull(
        VkCommandBuffer commands,
        std::size_t frame,
        float aspect,
        const PresentationGraphPlan& graph) const;
    void indirectReadback(
        VkCommandBuffer commands, std::size_t frame, const PresentationGraphPlan& graph) const;
    std::expected<void, core::Error> readVisibility(std::size_t frame, FrameScreenshot& data);
    void abandon() noexcept;

private:
    std::expected<std::shared_ptr<Mesh>, core::Error> upload(const StaticMeshUpload& input);

    DeviceResources& m_resources;
    SetBindingTable* m_bindings;
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
