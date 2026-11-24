#include "StaticSceneDraw.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

#include <vk_mem_alloc.h>

#include "binding/DescriptorHeapTable.h"
#include "binding/SetBindingTable.h"

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

std::array<float, 16>
multiplyMat4(const std::array<float, 16>& left, const std::array<float, 16>& right) noexcept {
    std::array<float, 16> out{};
    for (std::size_t col = 0; col < 4; ++col) {
        for (std::size_t row = 0; row < 4; ++row) {
            out[col * 4 + row] =
                left[0 * 4 + row] * right[col * 4 + 0] + left[1 * 4 + row] * right[col * 4 + 1] +
                left[2 * 4 + row] * right[col * 4 + 2] + left[3 * 4 + row] * right[col * 4 + 3];
        }
    }
    return out;
}

std::array<float, 12> multiplyViewNormal(
    const std::array<float, 16>& view, const std::array<float, 12>& worldNormal) noexcept {
    std::array<float, 12> out = worldNormal;
    for (std::size_t col = 0; col < 3; ++col) {
        for (std::size_t row = 0; row < 3; ++row) {
            out[col * 4 + row] = view[0 * 4 + row] * worldNormal[col * 4 + 0] +
                                 view[1 * 4 + row] * worldNormal[col * 4 + 1] +
                                 view[2 * 4 + row] * worldNormal[col * 4 + 2];
        }
    }
    return out;
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
        bool retirePublished = false;
        bool retireHeapPublished = false;
        for (const auto& texture : textures) {
            if (texture.binding) {
                retirePublished |= owner.m_bindings->resolveImage(*texture.binding).has_value();
                owner.m_bindings->retireImage(*texture.binding);
            }
            if (texture.heapBinding) {
                retireHeapPublished |=
                    owner.m_heapBindings->resolveImage(*texture.heapBinding).has_value();
                owner.m_heapBindings->retireImage(*texture.heapBinding);
            }
        }
        // Published meshes are destroyed after Presentation has drained their submissions.
        if (retirePublished) {
            owner.m_bindings->publishCompleted(true, true);
        }
        if (retireHeapPublished) {
            static_cast<void>(owner.m_heapBindings->publishCompleted(true));
        }
        for (const auto& texture : textures) {
            if (texture.sampled) {
                resources.releaseSampledView(texture.sampled);
            }
            if (texture.view.generation) {
                static_cast<void>(resources.destroyImageView(texture.view));
            }
            if (texture.image.generation) {
                static_cast<void>(resources.destroyImage(texture.image));
            }
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
    struct Texture {
        ImageHandle image;
        ImageViewHandle view;
        VkImageView sampled = VK_NULL_HANDLE;
        std::optional<SetBindingTable::ImageBinding> binding;
        std::optional<DescriptorHeapTable::ImageBinding> heapBinding;
    };
    std::array<Texture, 2> textures{};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::uint32_t indexCount = 0;
};

StaticSceneDraw::StaticSceneDraw(
    DeviceResources& resources,
    bool enableGBuffer,
    SetBindingTable* bindings,
    DescriptorHeapTable* heapBindings)
    : m_resources(resources), m_bindings(bindings), m_heapBindings(heapBindings),
      m_enableGBuffer(enableGBuffer) {}

StaticSceneDraw::~StaticSceneDraw() {
    m_active.clear();
    if (!m_abandoned) {
        destroyEnvironment(m_pendingEnvironment);
        if (m_sampler) {
            vkDestroySampler(m_resources.m_device, m_sampler, nullptr);
        }
        if (m_layout) {
            vkDestroyDescriptorSetLayout(m_resources.m_device, m_layout, nullptr);
        }
    }
}

std::expected<void, core::Error> StaticSceneDraw::open(const StaticIndirectShaders* indirect) {
    if (indirect) {
        m_indirect.emplace(m_resources, m_heapBindings);
        return m_indirect->open(*indirect);
    }
    if (m_bindings || m_heapBindings) {
        return {};
    }
    const std::array bindings{
        VkDescriptorSetLayoutBinding{
            0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        VkDescriptorSetLayoutBinding{
            1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = m_enableGBuffer ? 2 : 1;
    layout.pBindings = bindings.data();
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

VkDescriptorSetLayout StaticSceneDraw::layout() const noexcept {
    return m_bindings ? m_bindings->layout() : m_layout;
}

VkDescriptorSetLayout StaticSceneDraw::indirectLayout() const noexcept {
    return m_indirect ? m_indirect->layout() : VK_NULL_HANDLE;
}

std::expected<StaticSceneDraw::Attachment, core::Error> StaticSceneDraw::createAttachment(
    VkExtent2D extent,
    VkFormat format,
    VkImageAspectFlags aspect,
    VkImageUsageFlags usage,
    std::uint32_t mipCount,
    bool cube) {
    const std::uint32_t layers = cube ? 6u : 1u;
    const VkImageCreateFlags flags =
        cube ? VkImageCreateFlags{VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT} : 0;
    VkImageFormatProperties properties{};
    const auto supported = vkGetPhysicalDeviceImageFormatProperties(
        m_resources.m_physical,
        format,
        VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        usage,
        flags,
        &properties);
    if (supported != VK_SUCCESS) {
        return failed("static scene attachment format and usage unavailable", supported);
    }
    if (!extent.width || !extent.height || extent.width > properties.maxExtent.width ||
        extent.height > properties.maxExtent.height || !mipCount ||
        mipCount > properties.maxMipLevels || layers > properties.maxArrayLayers ||
        (cube && extent.width != extent.height)) {
        return invalid("static scene attachment extent is out of range");
    }
    if (auto ready = m_resources.ensureAllocator(); !ready) {
        return std::unexpected(ready.error());
    }
    Attachment attachment;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format;
    image.extent = {extent.width, extent.height, 1};
    image.flags = flags;
    image.mipLevels = mipCount;
    image.arrayLayers = layers;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = usage;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (auto result = vmaCreateImage(
            m_resources.m_allocator, &image, &allocation,
            &attachment.image, &attachment.allocation, nullptr);
        result != VK_SUCCESS) {
        return failed("allocate static scene attachment", result);
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = attachment.image;
    view.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, mipCount, 0, layers};
    if (auto result = vkCreateImageView(m_resources.m_device, &view, nullptr, &attachment.view);
        result != VK_SUCCESS) {
        destroyAttachment(attachment);
        return failed("create static scene attachment view", result);
    }
    return attachment;
}

void StaticSceneDraw::destroyAttachment(Attachment& attachment) noexcept {
    if (attachment.view) {
        vkDestroyImageView(m_resources.m_device, attachment.view, nullptr);
    }
    if (attachment.image) {
        vmaDestroyImage(m_resources.m_allocator, attachment.image, attachment.allocation);
    }
    attachment = {};
}

std::expected<StaticSceneDraw::UniformBuffer, core::Error>
StaticSceneDraw::createUniformBuffer(std::size_t bytes) {
    if (bytes == 0) {
        return invalid("uniform buffer size must be non-zero");
    }
    if (auto ready = m_resources.ensureAllocator(); !ready) {
        return std::unexpected(ready.error());
    }
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        (m_heapBindings ? static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
                        : 0);
    auto allocated = m_resources.allocateBuffer({bytes, BufferMemory::HostVisible}, usage);
    if (!allocated) {
        return std::unexpected(allocated.error());
    }
    UniformBuffer result{allocated->buffer, allocated->allocation, 0};
    if (m_heapBindings) {
        auto getBufferDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
            vkGetDeviceProcAddr
                ? vkGetDeviceProcAddr(m_resources.m_device, "vkGetBufferDeviceAddress")
                : nullptr);
        if (!getBufferDeviceAddress) {
            getBufferDeviceAddress = vkGetBufferDeviceAddress;
        }
        if (!getBufferDeviceAddress) {
            destroyUniformBuffer(result);
            return std::unexpected(
                core::Error{
                    core::ErrorCategory::LoaderSymbolMissing,
                    core::ErrorStage::Capability,
                    "vkGetBufferDeviceAddress entry point is unavailable",
                    std::nullopt});
        }
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = result.buffer;
        result.address = getBufferDeviceAddress(m_resources.m_device, &addressInfo);
        if (result.address == 0) {
            destroyUniformBuffer(result);
            return std::unexpected(
                core::Error{
                    core::ErrorCategory::InitializationFailed,
                    core::ErrorStage::Capability,
                    "uniform buffer has no device address",
                    std::nullopt});
        }
    }
    return result;
}

std::expected<void, core::Error> StaticSceneDraw::writeUniformBuffer(
    const UniformBuffer& buffer, std::span<const std::byte> data) {
    if (!buffer.allocation) {
        return invalid("uniform buffer allocation is null");
    }
    return m_resources.writeAllocation(buffer.allocation, 0, data);
}

void StaticSceneDraw::destroyUniformBuffer(UniformBuffer& buffer) noexcept {
    if (buffer.buffer) {
        vmaDestroyBuffer(m_resources.m_allocator, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

void StaticSceneDraw::destroyEnvironment(Environment& environment) noexcept {
    for (auto& attachment : environment) {
        destroyAttachment(attachment);
    }
}

std::expected<StaticSceneDraw::Environment, core::Error>
StaticSceneDraw::prepareEnvironment(const EnvironmentUpload& upload, bool verifyRollback) {
    struct Texture {
        const char* name;
        std::span<const std::uint16_t> texels;
        VkFormat format;
        std::uint32_t extent;
        std::uint32_t mips;
        std::uint32_t layers;
        std::uint32_t channels;
    };
    const std::array textures{
        Texture{"diffuse", upload.diffuse, VK_FORMAT_R16G16B16A16_SFLOAT, 16, 1, 6, 4},
        Texture{"specular", upload.specular, VK_FORMAT_R16G16B16A16_SFLOAT, 16, 5, 6, 4},
        Texture{"BRDF", upload.brdf, VK_FORMAT_R16G16_SFLOAT, 32, 1, 1, 2},
    };
    if (auto ready = m_resources.settleTransfer(true); !ready) {
        return std::unexpected(ready.error());
    }
    destroyEnvironment(m_pendingEnvironment);
    Environment candidate{};
    const auto rollback = [&](core::Error error, const Texture& texture) {
        error.message = std::string("environment ") + texture.name + ": " + error.message;
        if (m_resources.hasPendingTransfer()) {
            m_pendingEnvironment = candidate;
        } else {
            destroyEnvironment(candidate);
        }
        return std::unexpected(std::move(error));
    };
    for (std::size_t i = 0; i < textures.size(); ++i) {
        const auto& texture = textures[i];
        const auto bytes = std::as_bytes(texture.texels);
        std::vector<VkBufferImageCopy> regions;
        regions.reserve(texture.mips * texture.layers);
        VkDeviceSize offset = 0;
        for (std::uint32_t mip = 0; mip < texture.mips; ++mip) {
            const auto extent = texture.extent >> mip;
            for (std::uint32_t layer = 0; layer < texture.layers; ++layer) {
                VkBufferImageCopy copy{};
                copy.bufferOffset = offset;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
                copy.imageExtent = {extent, extent, 1};
                regions.push_back(copy);
                offset += VkDeviceSize{extent} * extent * texture.channels * sizeof(std::uint16_t);
            }
        }
        if (offset != bytes.size()) {
            return rollback(invalid("unexpected packed subresource size").error(), texture);
        }
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_resources.m_physical, texture.format, &properties);
        if (!(properties.optimalTilingFeatures &
              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
            return rollback(invalid("format does not support linear sampling").error(), texture);
        }
        auto image = createAttachment(
            {texture.extent, texture.extent},
            texture.format,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            texture.mips,
            texture.layers == 6);
        if (!image) {
            return rollback(image.error(), texture);
        }
        candidate[i] = *image;
        DeviceResources::ImageSlot target{};
        target.image = image->image;
        auto submitted = m_resources.transfer(
            VK_NULL_HANDLE,
            &target,
            bytes,
            {},
            false,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            regions);
        if (!submitted) {
            return rollback(submitted.error(), texture);
        }
        if (auto completed = m_resources.waitTransfer(*submitted); !completed) {
            return rollback(completed.error(), texture);
        }
        if (verifyRollback && i == 0) {
            return rollback(invalid("injected failure after diffuse upload").error(), texture);
        }
    }
    return candidate;
}

std::expected<std::shared_ptr<StaticSceneDraw::Mesh>, core::Error>
StaticSceneDraw::upload(const StaticMeshUpload& input) {
    const bool hasPerMeshGeometry = !input.vertices.empty() || !input.indices.empty();
    if (!input.identity || (!m_indirect && !hasPerMeshGeometry) ||
        (hasPerMeshGeometry &&
         (input.vertices.empty() || input.indices.empty() ||
          input.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
          input.indices.size() % 3 || std::ranges::any_of(input.indices, [&](std::uint32_t index) {
              return index >= input.vertices.size();
          })))) {
        return invalid("invalid static mesh topology or identity");
    }
    auto mesh = std::make_shared<Mesh>(*this);
    mesh->identity = input.identity;
    mesh->indexCount = m_indirect ? 0u : static_cast<std::uint32_t>(input.indices.size());
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
    if (!m_indirect) {
        if (auto made = fill(
                mesh->vertices, std::as_bytes(input.vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            !made) {
            return std::unexpected(made.error());
        }
        if (auto made =
                fill(mesh->indices, std::as_bytes(input.indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            !made) {
            return std::unexpected(made.error());
        }
    }
    const std::uint32_t textureCount = m_enableGBuffer ? 2 : 1;
    const std::array descriptions{
        ImageDesc{input.width, input.height}, ImageDesc{input.materialWidth, input.materialHeight}};
    const std::array pixels{input.rgba, input.materialRgba};
    for (std::uint32_t i = 0; i < textureCount; ++i) {
        auto& texture = mesh->textures[i];
        auto image = m_resources.createSampledImage(descriptions[i], std::as_bytes(pixels[i]));
        if (!image) {
            return std::unexpected(image.error());
        }
        texture.image = *image;
        if (m_heapBindings) {
            auto binding = m_heapBindings->registerImage(*image);
            if (!binding) {
                return std::unexpected(std::move(binding.error()));
            }
            texture.heapBinding = *binding;
            continue;
        }
        auto view = m_resources.createImageView(*image);
        if (!view) {
            return std::unexpected(view.error());
        }
        texture.view = *view;
        auto sampled = m_resources.retainSampledView(*view);
        if (!sampled) {
            return std::unexpected(sampled.error());
        }
        if (m_bindings) {
            texture.binding = m_bindings->registerImage(*sampled);
            if (!texture.binding) {
                m_resources.releaseSampledView(*sampled);
                return invalid("static scene sampled image table capacity exhausted");
            }
        } else {
            texture.sampled = *sampled;
        }
    }
    if (m_bindings || m_heapBindings) {
        return mesh;
    }
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, textureCount};
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
    std::array<VkDescriptorImageInfo, 2> textures{};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t i = 0; i < textureCount; ++i) {
        textures[i] = {m_sampler, mesh->textures[i].sampled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        auto& write = writes[i];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = mesh->set;
        write.dstBinding = i;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &textures[i];
    }
    vkUpdateDescriptorSets(device, textureCount, writes.data(), 0, nullptr);
    return mesh;
}

std::expected<std::vector<StaticSceneDraw::Draw>, core::Error> StaticSceneDraw::prepare(
    std::span<const StaticMeshUpload> meshes, std::span<const StaticInstanceDraw> draws,
    bool verifyRollback) {
    if (std::ranges::any_of(draws, [&](const auto& draw) {
            const auto finite = [](float value) { return std::isfinite(value); };
            return draw.meshIndex >= meshes.size() ||
                   !std::ranges::all_of(draw.modelMatrix, finite) ||
                   (m_enableGBuffer && (!std::ranges::all_of(draw.normalFromModel, finite) ||
                                        !std::ranges::all_of(draw.baseColorFactor, finite)));
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
        prepared.push_back({mesh->second, draw});
    }
    if (verifyRollback) {
        return invalid("rollback fixture did not reach a new mesh upload");
    }
    return prepared;
}

std::expected<std::shared_ptr<StaticSceneIndirect::Scene>, core::Error>
StaticSceneDraw::prepareIndirect(
    std::span<const Draw> draws, const StaticIndirectGeometry* geometry) {
    if (!m_indirect) {
        return nullptr;
    }
    if (!geometry || geometry->objectIds.size() != draws.size()) {
        return invalid("missing static indirect geometry");
    }
    std::vector<StaticSceneIndirect::Instance> instances;
    std::vector<StaticSceneIndirect::Material> materials;
    instances.reserve(draws.size());
    materials.reserve(draws.size());
    for (const auto& draw : draws) {
        const auto& instance = draw.instance;
        if (instance.meshIndex >= geometry->ranges.size()) {
            return invalid("static indirect mesh range is invalid");
        }
        const auto& range = geometry->ranges[instance.meshIndex];
        const auto material = static_cast<std::uint32_t>(materials.size());
        const auto textureSlot = [&](std::size_t index) {
            return m_heapBindings
                       ? static_cast<std::uint32_t>(draw.mesh->textures[index].heapBinding->slot)
                       : static_cast<std::uint32_t>(draw.mesh->textures[index].binding->slot);
        };
        materials.push_back(
            {instance.baseColorFactor,
             instance.normalFromModel[3],
             instance.normalFromModel[7],
             textureSlot(0),
             textureSlot(1)});
        instances.push_back(
            {instance.modelMatrix,
             instance.normalFromModel,
             {range.minimum[0], range.minimum[1], range.minimum[2], 0.F},
             {range.maximum[0], range.maximum[1], range.maximum[2], 0.F},
             range.firstIndex,
             range.indexCount,
             range.vertexOffset,
             material});
    }
    return m_indirect->prepare(*geometry, instances, materials);
}

std::expected<void, core::Error> StaticSceneDraw::publish(
    std::vector<Draw> draws, std::shared_ptr<StaticSceneIndirect::Scene> indirect) {
    if (m_bindings) {
        m_bindings->publishCompleted(true, true);
    }
    if (m_heapBindings) {
        if (auto published = m_heapBindings->publishCompleted(true); !published) {
            return std::unexpected(std::move(published.error()));
        }
    }
    if (m_indirect && indirect) {
        if (auto written = m_indirect->publishHeapBuffers(*indirect); !written) {
            if (m_indirectScene) {
                static_cast<void>(m_indirect->publishHeapBuffers(*m_indirectScene));
            }
            return std::unexpected(std::move(written.error()));
        }
    }
    m_indirectScene = std::move(indirect);
    m_active = std::move(draws);
    return {};
}

std::expected<void, core::Error>
StaticSceneDraw::updateFrameView(std::size_t frame, const FrameView& view, float inverseAspect) {
    if (m_indirect && m_indirectScene) {
        return m_indirect->updateFrameView(*m_indirectScene, frame, view, inverseAspect);
    }
    return {};
}

void StaticSceneDraw::cull(
    VkCommandBuffer commands, std::size_t frame, const PresentationGraphPlan& graph) const {
    if (m_indirectScene) {
        m_indirect->cull(*m_indirectScene, commands, frame, graph);
    }
}

void StaticSceneDraw::indirectReadback(
    VkCommandBuffer commands, std::size_t frame, const PresentationGraphPlan& graph) const {
    if (m_indirectScene) {
        m_indirect->barrier(*m_indirectScene, commands, frame, graph, IndirectPhase::Readback);
    }
}

std::expected<void, core::Error>
StaticSceneDraw::readVisibility(std::size_t frame, FrameScreenshot& data) {
    if (m_indirectScene) {
        auto visibility = m_indirect->visibility(*m_indirectScene, frame);
        if (!visibility) {
            return std::unexpected(visibility.error());
        }
        data.indirect = std::move(*visibility);
    }
    return {};
}

void StaticSceneDraw::abandon() noexcept {
    m_abandoned = true;
    if (m_indirect) {
        m_indirect->abandon();
    }
}

void StaticSceneDraw::record(
    VkCommandBuffer commands,
    VkPipelineLayout layout,
    const FrameView& view,
    float inverseAspect,
    std::size_t frame) const {
    if (m_indirect) {
        if (m_indirectScene) {
            m_indirect->draw(
                *m_indirectScene,
                commands,
                frame,
                layout,
                m_bindings ? m_bindings->set() : VK_NULL_HANDLE);
        }
        return;
    }
    constexpr VkDeviceSize offset = 0;
    if (m_bindings) {
        const auto set = m_bindings->set();
        vkCmdBindDescriptorSets(
            commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    }
    auto viewProj = view.clipFromWorld;
    for (std::size_t column = 0; column < 4; ++column) {
        viewProj[column * 4] *= inverseAspect;
    }
    for (const auto& draw : m_active) {
        const auto matrix = multiplyMat4(viewProj, draw.instance.modelMatrix);
        if (m_enableGBuffer) {
            const auto stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            auto normal = multiplyViewNormal(view.view, draw.instance.normalFromModel);
            if (m_bindings) {
                // Two 8-bit slots fit exactly in a float and leave the normal columns unchanged.
                static_assert(kStaticSceneImageLimit <= 256);
                const auto slots = draw.mesh->textures[0].binding->slot |
                                   (draw.mesh->textures[1].binding->slot << 8);
                normal[11] = static_cast<float>(slots);
            } else if (m_heapBindings) {
                static_assert(kHeapSceneImageLimit <= 256);
                const auto slots = draw.mesh->textures[0].heapBinding->slot |
                                   (draw.mesh->textures[1].heapBinding->slot << 8);
                normal[11] = static_cast<float>(slots);
            }
            if (m_heapBindings) {
                struct DirectGBufferPush {
                    std::array<float, 16> clipFromModel;
                    std::array<float, 12> normalColumns;
                    std::array<float, 4> baseColorFactor;
                };
                static_assert(sizeof(DirectGBufferPush) == 128);
                const DirectGBufferPush push{matrix, normal, draw.instance.baseColorFactor};
                VkPushDataInfoEXT pushInfo{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
                pushInfo.offset = 0;
                pushInfo.data = {&push, sizeof(push)};
                m_heapBindings->dispatch().cmdPushData(commands, &pushInfo);
            } else {
                vkCmdPushConstants(commands, layout, stages, 0, sizeof(matrix), matrix.data());
                vkCmdPushConstants(
                    commands, layout, stages, sizeof(matrix), sizeof(normal), normal.data());
                vkCmdPushConstants(
                    commands,
                    layout,
                    stages,
                    sizeof(matrix) + sizeof(draw.instance.normalFromModel),
                    sizeof(draw.instance.baseColorFactor),
                    draw.instance.baseColorFactor.data());
            }
        } else {
            vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(matrix), matrix.data());
        }
        vkCmdBindVertexBuffers(commands, 0, 1, &draw.mesh->vertices.buffer, &offset);
        vkCmdBindIndexBuffer(commands, draw.mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        if (!m_bindings && !m_heapBindings) {
            vkCmdBindDescriptorSets(
                commands,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                layout,
                0,
                1,
                &draw.mesh->set,
                0,
                nullptr);
        }
        vkCmdDrawIndexed(commands, draw.mesh->indexCount, 1, 0, 0, 0);
    }
}

void StaticSceneDraw::recordShadow(
    VkCommandBuffer commands,
    VkPipelineLayout layout,
    const std::array<float, 16>& lightViewProj,
    std::size_t frame) const {
    if (m_indirect) {
        if (m_indirectScene) {
            m_indirect->drawShadow(*m_indirectScene, commands, frame, layout, lightViewProj);
        }
        return;
    }
    constexpr VkDeviceSize offset = 0;
    for (const auto& draw : m_active) {
        const auto lightClipFromModel = multiplyMat4(lightViewProj, draw.instance.modelMatrix);
        if (m_heapBindings) {
            VkPushDataInfoEXT pushInfo{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
            pushInfo.offset = 0;
            pushInfo.data = {lightClipFromModel.data(), sizeof(lightClipFromModel)};
            m_heapBindings->dispatch().cmdPushData(commands, &pushInfo);
        } else {
            vkCmdPushConstants(
                commands,
                layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(lightClipFromModel),
                lightClipFromModel.data());
        }
        vkCmdBindVertexBuffers(commands, 0, 1, &draw.mesh->vertices.buffer, &offset);
        vkCmdBindIndexBuffer(commands, draw.mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(commands, draw.mesh->indexCount, 1, 0, 0, 0);
    }
}

} // namespace ar::rhi::detail
