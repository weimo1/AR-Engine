#include "StaticSceneIndirect.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include <vk_mem_alloc.h>

namespace ar::rhi::detail {
namespace {

static_assert(sizeof(StaticSceneIndirect::Instance) == 160);
static_assert(offsetof(StaticSceneIndirect::Instance, normal) == 64);
static_assert(offsetof(StaticSceneIndirect::Instance, minimum) == 112);
static_assert(offsetof(StaticSceneIndirect::Instance, maximum) == 128);
static_assert(offsetof(StaticSceneIndirect::Instance, firstIndex) == 144);
static_assert(offsetof(StaticSceneIndirect::Instance, material) == 156);
static_assert(sizeof(StaticSceneIndirect::Material) == 32);
static_assert(offsetof(StaticSceneIndirect::Material, metallic) == 16);
static_assert(offsetof(StaticSceneIndirect::Material, albedo) == 24);
static_assert(sizeof(VkDrawIndexedIndirectCommand) == 20);
static_assert(offsetof(VkDrawIndexedIndirectCommand, firstInstance) == 16);

std::unexpected<core::Error> failure(const char* operation, VkResult result) {
    return std::unexpected(
        core::Error{
            core::ErrorCategory::InitializationFailed,
            core::ErrorStage::Capability,
            operation,
            core::ErrorOrigin{"vulkan", result}});
}

std::unexpected<core::Error> invalid(const char* reason) {
    return std::unexpected(
        core::Error{
            core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation, reason, {}});
}

struct Scope {
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};

Scope scope(IndirectUse use) {
    switch (use) {
    case IndirectUse::Clear:
        return {VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case IndirectUse::ComputeWrite:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case IndirectUse::ComputeReadWrite:
        return {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case IndirectUse::DrawRead:
        return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
    case IndirectUse::HostRead:
        return {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
    }
    std::unreachable();
}

} // namespace

StaticSceneIndirect::~StaticSceneIndirect() {
    if (!m_abandoned) {
        const auto device = m_resources.m_device;
        vkDestroyPipeline(device, m_pipeline, nullptr);
        vkDestroyPipelineLayout(device, m_computeLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
    }
}

StaticSceneIndirect::Scene::~Scene() {
    if (owner.m_abandoned) {
        return;
    }
    auto& resources = owner.m_resources;
    vkDestroyDescriptorPool(resources.m_device, pool, nullptr);
    const auto destroy = [&](const Buffer& buffer) {
        if (buffer.buffer) {
            vmaDestroyBuffer(resources.m_allocator, buffer.buffer, buffer.allocation);
        }
    };
    for (const auto& frame : frames) {
        destroy(frame.commands);
        destroy(frame.counts);
    }
    destroy(vertices);
    destroy(indices);
    destroy(instances);
    destroy(materials);
}

std::expected<void, core::Error> StaticSceneIndirect::open(const StaticIndirectShaders& shaders) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_resources.m_physical, &properties);
    m_limits = properties.limits;
    m_capacity = std::min(shaders.capacity, m_limits.maxDrawIndirectCount);
    constexpr std::array bindings{
        VkDescriptorSetLayoutBinding{
            0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            1,
            VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT,
            nullptr},
        VkDescriptorSetLayoutBinding{
            1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        VkDescriptorSetLayoutBinding{
            2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{
            3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    const auto device = m_resources.m_device;
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = static_cast<std::uint32_t>(bindings.size());
    set.pBindings = bindings.data();
    if (auto code = vkCreateDescriptorSetLayout(device, &set, nullptr, &m_setLayout);
        code != VK_SUCCESS) {
        return failure("create static indirect descriptor layout", code);
    }
    struct ComputePush {
        float aspect;
        std::uint32_t count, capacity;
    };
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m_setLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (auto code = vkCreatePipelineLayout(device, &layout, nullptr, &m_computeLayout);
        code != VK_SUCCESS) {
        return failure("create static indirect compute layout", code);
    }
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = shaders.compute.size() * sizeof(std::uint32_t);
    moduleInfo.pCode = shaders.compute.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (auto code = vkCreateShaderModule(device, &moduleInfo, nullptr, &module);
        code != VK_SUCCESS) {
        return failure("create static indirect compute shader", code);
    }
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr,
        0,
        VK_SHADER_STAGE_COMPUTE_BIT,
        module,
        "main",
        nullptr};
    pipeline.layout = m_computeLayout;
    const auto code = vkCreateComputePipelines(device, {}, 1, &pipeline, nullptr, &m_pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (code != VK_SUCCESS) {
        return failure("create static indirect compute pipeline", code);
    }
    return {};
}

std::expected<std::shared_ptr<StaticSceneIndirect::Scene>, core::Error>
StaticSceneIndirect::prepare(
    const StaticIndirectGeometry& geometry,
    std::span<const Instance> instances,
    std::span<const Material> materials) {
    if (instances.size() != geometry.objectIds.size() ||
        (instances.size() + 63) / 64 > m_limits.maxComputeWorkGroupCount[0] ||
        instances.size_bytes() > m_limits.maxStorageBufferRange ||
        materials.size_bytes() > m_limits.maxStorageBufferRange) {
        return invalid("static indirect scene exceeds buffer or dispatch limits");
    }
    for (const auto& range : geometry.ranges) {
        if (range.vertexOffset < 0 || range.indexCount == 0 || range.indexCount % 3 != 0 ||
            range.firstIndex > geometry.indices.size() ||
            range.indexCount > geometry.indices.size() - range.firstIndex) {
            return invalid("static indirect mesh has an invalid range");
        }
        for (std::size_t axis = 0; axis < range.minimum.size(); ++axis) {
            if (!std::isfinite(range.minimum[axis]) || !std::isfinite(range.maximum[axis]) ||
                range.minimum[axis] > range.maximum[axis]) {
                return invalid("static indirect mesh has invalid bounds");
            }
        }
        for (auto index : geometry.indices.subspan(range.firstIndex, range.indexCount)) {
            if (static_cast<std::uint64_t>(range.vertexOffset) + index >=
                geometry.vertices.size()) {
                return invalid("static indirect vertex index is out of range");
            }
        }
    }
    auto scene = std::make_shared<Scene>(*this);
    scene->objectIds.assign(geometry.objectIds.begin(), geometry.objectIds.end());
    scene->capacity = std::min(m_capacity, static_cast<std::uint32_t>(instances.size()));
    const auto commandsSize = std::max(1u, scene->capacity) * sizeof(VkDrawIndexedIndirectCommand);
    if (commandsSize > m_limits.maxStorageBufferRange) {
        return invalid("static indirect command buffer exceeds device range");
    }
    const auto fill =
        [&](Buffer& target,
            VkBufferUsageFlags usage,
            std::size_t size,
            std::span<const std::byte> data = {}) -> std::expected<void, core::Error> {
        auto buffer = m_resources.allocateBuffer(
            {std::max<std::size_t>(size, 4), BufferMemory::HostVisible}, usage);
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        target = *buffer;
        return data.empty() ? std::expected<void, core::Error>{}
                            : m_resources.writeAllocation(target.allocation, 0, data);
    };
    constexpr auto storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr auto indirect = storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    struct Upload {
        Buffer* target;
        VkBufferUsageFlags usage;
        std::span<const std::byte> data;
    };
    const std::array uploads{
        Upload{
            &scene->vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, std::as_bytes(geometry.vertices)},
        Upload{&scene->indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, std::as_bytes(geometry.indices)},
        Upload{&scene->instances, storage, std::as_bytes(instances)},
        Upload{&scene->materials, storage, std::as_bytes(materials)}};
    for (const auto& upload : uploads) {
        if (auto result = fill(*upload.target, upload.usage, upload.data.size(), upload.data);
            !result) {
            return std::unexpected(result.error());
        }
    }
    const auto device = m_resources.m_device;
    constexpr VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 2;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (auto code = vkCreateDescriptorPool(device, &pool, nullptr, &scene->pool);
        code != VK_SUCCESS) {
        return failure("create static indirect descriptor pool", code);
    }
    for (auto& frame : scene->frames) {
        if (auto made = fill(frame.commands, indirect, commandsSize); !made) {
            return std::unexpected(made.error());
        }
        if (auto made = fill(frame.counts, indirect, sizeof(std::uint32_t) * 2); !made) {
            return std::unexpected(made.error());
        }
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = scene->pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &m_setLayout;
        if (auto code = vkAllocateDescriptorSets(device, &allocate, &frame.set);
            code != VK_SUCCESS) {
            return failure("allocate static indirect descriptor set", code);
        }
        const std::array buffers{
            scene->instances.buffer,
            scene->materials.buffer,
            frame.commands.buffer,
            frame.counts.buffer};
        std::array<VkDescriptorBufferInfo, buffers.size()> info{};
        std::array<VkWriteDescriptorSet, buffers.size()> writes{};
        for (std::uint32_t binding = 0; binding < buffers.size(); ++binding) {
            info[binding] = {buffers[binding], 0, VK_WHOLE_SIZE};
            writes[binding] = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                frame.set,
                binding,
                0,
                1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr,
                &info[binding],
                nullptr};
        }
        vkUpdateDescriptorSets(
            device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    return scene;
}

void StaticSceneIndirect::barrier(
    const Scene& scene,
    VkCommandBuffer command,
    std::size_t slot,
    const PresentationGraphPlan& graph,
    IndirectPhase phase) const {
    const auto& frame = scene.frames[slot];
    std::array<VkBufferMemoryBarrier2, 2> barriers{
        VkBufferMemoryBarrier2{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2},
        VkBufferMemoryBarrier2{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2}};
    const std::array buffers{frame.commands.buffer, frame.counts.buffer};
    for (const auto& item : graph.indirectBarriers[std::to_underlying(phase)]) {
        const auto index = std::to_underlying(item.resource);
        auto& barrier = barriers[index];
        const auto source = scope(item.source);
        const auto destination = scope(item.destination);
        barrier.srcStageMask |= source.stage;
        barrier.srcAccessMask |= source.access;
        barrier.dstStageMask |= destination.stage;
        barrier.dstAccessMask |= destination.access;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffers[index];
        barrier.size = VK_WHOLE_SIZE;
    }
    auto used = std::ranges::remove_if(barriers, [](const auto& item) { return !item.buffer; });
    if (used.begin() != barriers.begin()) {
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount =
            static_cast<std::uint32_t>(used.begin() - barriers.begin());
        dependency.pBufferMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(command, &dependency);
    }
}

void StaticSceneIndirect::cull(
    const Scene& scene,
    VkCommandBuffer command,
    std::size_t slot,
    float aspect,
    const PresentationGraphPlan& graph) const {
    const auto& frame = scene.frames[slot];
    vkCmdFillBuffer(command, frame.counts.buffer, 0, VK_WHOLE_SIZE, 0);
    if (!scene.objectIds.empty()) {
        barrier(scene, command, slot, graph, IndirectPhase::Cull);
        struct {
            float aspect;
            std::uint32_t count, capacity;
        } push{aspect, static_cast<std::uint32_t>(scene.objectIds.size()), scene.capacity};
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_COMPUTE, m_computeLayout, 0, 1, &frame.set, 0, nullptr);
        vkCmdPushConstants(
            command, m_computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(command, (push.count + 63) / 64, 1, 1);
    }
    barrier(scene, command, slot, graph, IndirectPhase::Geometry);
}

void StaticSceneIndirect::draw(
    const Scene& scene,
    VkCommandBuffer command,
    std::size_t slot,
    float aspect,
    VkPipelineLayout layout,
    VkDescriptorSet textures) const {
    if (!scene.capacity) {
        return;
    }
    const auto& frame = scene.frames[slot];
    const std::array sets{textures, frame.set};
    vkCmdBindDescriptorSets(
        command,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        layout,
        0,
        static_cast<std::uint32_t>(sets.size()),
        sets.data(),
        0,
        nullptr);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(aspect), &aspect);
    constexpr VkDeviceSize start = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &scene.vertices.buffer, &start);
    vkCmdBindIndexBuffer(command, scene.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirectCount(
        command,
        frame.commands.buffer,
        0,
        frame.counts.buffer,
        sizeof(std::uint32_t),
        scene.capacity,
        sizeof(VkDrawIndexedIndirectCommand));
}

std::expected<FrameScreenshot::IndirectVisibility, core::Error>
StaticSceneIndirect::visibility(const Scene& scene, std::size_t slot) {
    const auto& frame = scene.frames[slot];
    std::array<std::uint32_t, 2> values{};
    if (auto read = m_resources.readAllocation(
            frame.counts.allocation, 0, std::as_writable_bytes(std::span(values)));
        !read) {
        return std::unexpected(read.error());
    }
    if (values[0] > scene.objectIds.size() || values[1] != std::min(values[0], scene.capacity)) {
        return invalid("GPU indirect counters are out of range");
    }
    FrameScreenshot::IndirectVisibility result{values[0], {}};
    if (values[1] == 0) {
        return result;
    }
    std::vector<VkDrawIndexedIndirectCommand> commands(values[1]);
    if (auto read = m_resources.readAllocation(
            frame.commands.allocation, 0, std::as_writable_bytes(std::span(commands)));
        !read) {
        return std::unexpected(read.error());
    }
    result.drawnIds.reserve(commands.size());
    for (const auto& command : commands) {
        if (command.firstInstance >= scene.objectIds.size() || command.instanceCount != 1) {
            return invalid("GPU indirect command has an invalid instance");
        }
        result.drawnIds.push_back(scene.objectIds[command.firstInstance]);
    }
    std::ranges::sort(result.drawnIds);
    return result;
}

} // namespace ar::rhi::detail
