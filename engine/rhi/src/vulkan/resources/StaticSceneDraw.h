#pragma once

#include "DeviceResources.h"

namespace ar::rhi::detail {

class StaticSceneDraw {
    struct Mesh;

public:
    explicit StaticSceneDraw(DeviceResources& resources, bool enableGBuffer);
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

    std::expected<void, core::Error> open();
    VkDescriptorSetLayout layout() const noexcept { return m_layout; }
    std::expected<Attachment, core::Error> createAttachment(
        VkExtent2D extent, VkFormat format, VkImageAspectFlags aspect, VkImageUsageFlags usage);
    void destroyAttachment(Attachment& attachment) noexcept;
    std::expected<std::vector<Draw>, core::Error> prepare(
        std::span<const StaticMeshUpload> meshes, std::span<const StaticInstanceDraw> draws,
        bool verifyRollback);
    void publish(std::vector<Draw> draws) noexcept;
    void record(VkCommandBuffer commands, VkPipelineLayout layout, float inverseAspect) const;
    void abandon() noexcept { m_abandoned = true; }

private:
    std::expected<std::shared_ptr<Mesh>, core::Error> upload(const StaticMeshUpload& input);

    DeviceResources& m_resources;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    std::vector<Draw> m_active;
    bool m_abandoned = false;
    bool m_enableGBuffer = false;
};

} // namespace ar::rhi::detail
