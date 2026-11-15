#pragma once

#include "DeviceResources.h"

namespace ar::rhi::detail {

class StaticSceneDraw {
    struct Mesh;

public:
    explicit StaticSceneDraw(DeviceResources& resources);
    ~StaticSceneDraw();
    StaticSceneDraw(const StaticSceneDraw&) = delete;

    static constexpr VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    struct Depth {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct Draw {
        std::shared_ptr<Mesh> mesh;
        std::array<float, 16> matrix;
    };

    std::expected<void, core::Error> open();
    VkDescriptorSetLayout layout() const noexcept { return m_layout; }
    std::expected<Depth, core::Error> createDepth(VkExtent2D extent);
    void destroyDepth(Depth& depth) noexcept;
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
};

} // namespace ar::rhi::detail
