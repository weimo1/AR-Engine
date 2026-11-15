#include "StaticSceneDraw.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

#include <vk_mem_alloc.h>

namespace ar::rhi::detail {
namespace {
std::unexpected<core::Error> failed(const char* operation, VkResult result) {
    return std::unexpected(core::Error{
        core::ErrorCategory::InitializationFailed, core::ErrorStage::Capability,
        operation, core::ErrorOrigin{"vulkan", result}});
}

std::unexpected<core::Error> invalid(const char* message) {
    return std::unexpected(core::Error{
        core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation, message, {}});
}
} // namespace

struct StaticSceneDraw::Mesh {
    explicit Mesh(StaticSceneDraw& draw) : owner(draw) {}
    ~Mesh() {
        if (owner.m_abandoned) {
            return;
        }
        auto& resources = owner.m_resources;
        if (pool) {
            vkDestroyDescriptorPool(resources.m_device, pool, nullptr);
        }
        if (sampled) {
            resources.releaseSampledView(sampled);
        }
        if (view.generation) {
            static_cast<void>(resources.destroyImageView(view));
        }
        if (image.generation) {
            static_cast<void>(resources.destroyImage(image));
        }
        for (const auto& buffer : {vertices, indices}) {
            if (buffer.buffer) {
                vmaDestroyBuffer(resources.m_allocator, buffer.buffer, buffer.allocation);
            }
        }
    }
    StaticSceneDraw& owner;
    std::shared_ptr<const void> identity;
    DeviceResources::BufferSlot vertices;
    DeviceResources::BufferSlot indices;
    ImageHandle image;
    ImageViewHandle view;
    VkImageView sampled = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::uint32_t indexCount = 0;
};

StaticSceneDraw::StaticSceneDraw(DeviceResources& resources) : m_resources(resources) {}

StaticSceneDraw::~StaticSceneDraw() {
    m_active.clear();
    if (!m_abandoned) {
        if (m_sampler) {
            vkDestroySampler(m_resources.m_device, m_sampler, nullptr);
        }
        if (m_layout) {
            vkDestroyDescriptorSetLayout(m_resources.m_device, m_layout, nullptr);
        }
    }
}

std::expected<void, core::Error> StaticSceneDraw::open() {
    VkFormatProperties depthProperties{};
    vkGetPhysicalDeviceFormatProperties(m_resources.m_physical, depthFormat, &depthProperties);
    if (!(depthProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        return failed("required static scene depth format unavailable", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 1;
    layout.pBindings = &binding;
    const auto device = m_resources.m_device;
    if (auto result = vkCreateDescriptorSetLayout(device, &layout, nullptr, &m_layout);
        result != VK_SUCCESS) {
        return failed("create static mesh descriptor layout", result);
    }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (auto result = vkCreateSampler(device, &sampler, nullptr, &m_sampler); result != VK_SUCCESS) {
        return failed("create static mesh sampler", result);
    }
    return {};
}

std::expected<StaticSceneDraw::Depth, core::Error> StaticSceneDraw::createDepth(VkExtent2D extent) {
    if (auto ready = m_resources.ensureAllocator(); !ready) {
        return std::unexpected(ready.error());
    }
    Depth depth;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = depthFormat;
    image.extent = {extent.width, extent.height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (auto result = vmaCreateImage(
            m_resources.m_allocator, &image, &allocation, &depth.image, &depth.allocation, nullptr);
        result != VK_SUCCESS) {
        return failed("allocate static scene depth attachment", result);
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = depth.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = depthFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (auto result = vkCreateImageView(m_resources.m_device, &view, nullptr, &depth.view);
        result != VK_SUCCESS) {
        destroyDepth(depth);
        return failed("create static scene depth view", result);
    }
    return depth;
}

void StaticSceneDraw::destroyDepth(Depth& depth) noexcept {
    if (depth.view) {
        vkDestroyImageView(m_resources.m_device, depth.view, nullptr);
    }
    if (depth.image) {
        vmaDestroyImage(m_resources.m_allocator, depth.image, depth.allocation);
    }
    depth = {};
}

std::expected<std::shared_ptr<StaticSceneDraw::Mesh>, core::Error>
StaticSceneDraw::upload(const StaticMeshUpload& input) {
    if (!input.identity || input.vertices.empty() || input.indices.empty() ||
        input.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
        input.indices.size() % 3 || std::ranges::any_of(input.indices, [&](std::uint32_t index) {
            return index >= input.vertices.size();
        })) {
        return invalid("invalid static mesh topology or identity");
    }
    auto mesh = std::make_shared<Mesh>(*this);
    mesh->identity = input.identity;
    mesh->indexCount = static_cast<std::uint32_t>(input.indices.size());
    if (auto ready = m_resources.ensureAllocator(); !ready) {
        return std::unexpected(ready.error());
    }
    const auto fill = [&](DeviceResources::BufferSlot& slot, std::span<const std::byte> data,
                          VkBufferUsageFlags usage) -> std::expected<void, core::Error> {
        auto buffer = m_resources.allocateBuffer({data.size(), BufferMemory::HostVisible}, usage);
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        slot = *buffer;
        return m_resources.writeAllocation(slot.allocation, 0, data);
    };
    if (auto made = fill(mesh->vertices, std::as_bytes(input.vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        !made) {
        return std::unexpected(made.error());
    }
    if (auto made = fill(mesh->indices, std::as_bytes(input.indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        !made) {
        return std::unexpected(made.error());
    }
    auto image = m_resources.createSampledImage({input.width, input.height}, std::as_bytes(input.rgba));
    if (!image) {
        return std::unexpected(image.error());
    }
    mesh->image = *image;
    auto view = m_resources.createImageView(*image);
    if (!view) {
        return std::unexpected(view.error());
    }
    mesh->view = *view;
    auto sampled = m_resources.retainSampledView(*view);
    if (!sampled) {
        return std::unexpected(sampled.error());
    }
    mesh->sampled = *sampled;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    const auto device = m_resources.m_device;
    if (auto result = vkCreateDescriptorPool(device, &pool, nullptr, &mesh->pool);
        result != VK_SUCCESS) {
        return failed("create static mesh descriptor pool", result);
    }
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = mesh->pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &m_layout;
    if (auto result = vkAllocateDescriptorSets(device, &allocate, &mesh->set); result != VK_SUCCESS) {
        return failed("allocate static mesh descriptor", result);
    }
    const VkDescriptorImageInfo texture{m_sampler, mesh->sampled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = mesh->set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &texture;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return mesh;
}

std::expected<std::vector<StaticSceneDraw::Draw>, core::Error> StaticSceneDraw::prepare(
    std::span<const StaticMeshUpload> meshes, std::span<const StaticInstanceDraw> draws,
    bool verifyRollback) {
    if (std::ranges::any_of(draws, [&](const auto& draw) {
            return draw.meshIndex >= meshes.size() ||
                   !std::ranges::all_of(draw.clipFromModel, [](float value) { return std::isfinite(value); });
        })) {
        return invalid("static scene draw has an invalid mesh index or matrix");
    }
    std::unordered_map<const void*, std::shared_ptr<Mesh>> resident;
    for (const auto& draw : m_active) {
        resident.try_emplace(draw.mesh->identity.get(), draw.mesh);
    }
    std::vector<Draw> prepared;
    prepared.reserve(draws.size());
    for (const auto& draw : draws) {
        const auto& input = meshes[draw.meshIndex];
        auto mesh = resident.find(input.identity.get());
        if (mesh == resident.end()) {
            auto uploaded = upload(input);
            if (!uploaded) {
                return std::unexpected(uploaded.error());
            }
            mesh = resident.emplace(input.identity.get(), std::move(*uploaded)).first;
            if (verifyRollback) {
                return invalid("injected failure after first candidate mesh upload");
            }
        }
        prepared.push_back({mesh->second, draw.clipFromModel});
    }
    if (verifyRollback) {
        return invalid("rollback fixture did not reach a new mesh upload");
    }
    return prepared;
}

void StaticSceneDraw::publish(std::vector<Draw> draws) noexcept {
    m_active = std::move(draws);
}

void StaticSceneDraw::record(VkCommandBuffer commands, VkPipelineLayout layout, float inverseAspect) const {
    constexpr VkDeviceSize offset = 0;
    for (const auto& draw : m_active) {
        auto matrix = draw.matrix;
        for (std::size_t column = 0; column < 4; ++column) {
            matrix[column * 4] *= inverseAspect;
        }
        vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(matrix), matrix.data());
        vkCmdBindVertexBuffers(commands, 0, 1, &draw.mesh->vertices.buffer, &offset);
        vkCmdBindIndexBuffer(commands, draw.mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                &draw.mesh->set, 0, nullptr);
        vkCmdDrawIndexed(commands, draw.mesh->indexCount, 1, 0, 0, 0);
    }
}

} // namespace ar::rhi::detail
