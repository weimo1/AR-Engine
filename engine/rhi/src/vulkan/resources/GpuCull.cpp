#include "DeviceResources.h"

#include <algorithm>
#include <string>

#include <vk_mem_alloc.h>

namespace ar::rhi::detail {
namespace {

core::Error cullError(std::string operation, VkResult result) {
    return {
        core::ErrorCategory::InitializationFailed,
        core::ErrorStage::Capability,
        std::move(operation),
        core::ErrorOrigin{"vulkan", result}};
}

} // namespace

void DeviceResources::clearCullResources() noexcept {
    vkDestroyPipeline(m_device, m_cullPipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_cullLayout, nullptr);
    vkDestroyDescriptorPool(m_device, m_cullPool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_cullSetLayout, nullptr);
    m_cullPipeline = {};
    m_cullLayout = {};
    m_cullPool = {};
    m_cullSetLayout = {};
    for (auto& buffer : m_cullBuffers) {
        if (buffer.buffer) {
            vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
            buffer = {};
        }
    }
}

std::expected<std::array<std::uint32_t, 2>, core::Error> DeviceResources::verifyGpuCull(
    std::span<const std::byte> planes,
    std::span<const std::byte> objects,
    std::span<const std::uint32_t> shader,
    std::span<std::uint32_t> ids,
    std::span<std::array<std::uint32_t, 4>> commands) {
    const auto invalid = [](const char* reason) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Capability,
                reason,
                std::nullopt});
    };
    if (planes.size() != 96 || objects.size() % 32 != 0 || ids.size() != commands.size() ||
        shader.empty() || objects.size() / 32 > UINT32_MAX - 63 || ids.size() > UINT32_MAX) {
        return invalid("invalid GPU culling input layout or capacity");
    }
    std::uint32_t queueCount = m_queueFamily + 1;
    std::vector<VkQueueFamilyProperties> queues(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &queueCount, queues.data());
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_physical, &properties);
    const auto& limits = properties.limits;
    const auto objectCount = static_cast<std::uint32_t>(objects.size() / 32);
    const auto groups = (objectCount + 63) / 64;
    const std::array<VkDeviceSize, 5> sizes{
        planes.size(),
        std::max<std::size_t>(32, objects.size()),
        std::max<std::size_t>(4, ids.size_bytes()),
        std::max<std::size_t>(16, commands.size_bytes()),
        8};
    if (m_queueFamily >= queueCount || !(queues[m_queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) ||
        groups > limits.maxComputeWorkGroupCount[0] || limits.maxComputeWorkGroupSize[0] < 64 ||
        limits.maxComputeWorkGroupInvocations < 64 ||
        limits.maxPerStageDescriptorStorageBuffers < 5 ||
        limits.maxDescriptorSetStorageBuffers < 5 ||
        std::ranges::any_of(
            sizes,
            [&](auto n) { return n > limits.maxStorageBufferRange || n > bufferSizeLimit(); })) {
        return invalid("GPU culling queue or device limits are insufficient");
    }
    if (auto done = settleTransfer(true); !done) {
        return std::unexpected(done.error());
    }
    if (auto ready = ensureAllocator(); !ready) {
        return std::unexpected(ready.error());
    }
    clearCullResources();
    if (auto ready = ensureTransferCommands(); !ready) {
        return std::unexpected(ready.error());
    }
    const auto run = [&]() -> std::expected<std::array<std::uint32_t, 2>, core::Error> {
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        std::array<VkDescriptorBufferInfo, 5> buffers{};
        for (std::uint32_t i = 0; i < bindings.size(); ++i) {
            const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               (i >= 3 ? VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT : 0);
            auto buffer = allocateBuffer({sizes[i], BufferMemory::HostVisible}, usage);
            if (!buffer) {
                return std::unexpected(buffer.error());
            }
            m_cullBuffers[i] = *buffer;
            bindings[i] = {
                i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            buffers[i] = {buffer->buffer, 0, sizes[i]};
        }
        const std::array inputs{planes, objects};
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (!inputs[i].empty()) {
                if (auto write = writeAllocation(m_cullBuffers[i].allocation, 0, inputs[i]);
                    !write) {
                    return std::unexpected(write.error());
                }
            }
        }
        VkDescriptorSetLayoutCreateInfo setInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        setInfo.pBindings = bindings.data();
        if (auto result =
                vkCreateDescriptorSetLayout(m_device, &setInfo, nullptr, &m_cullSetLayout);
            result != VK_SUCCESS) {
            return std::unexpected(cullError("vkCreateDescriptorSetLayout", result));
        }
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setInfo.bindingCount};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (auto result = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_cullPool);
            result != VK_SUCCESS) {
            return std::unexpected(cullError("vkCreateDescriptorPool", result));
        }
        VkDescriptorSetAllocateInfo setAllocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAllocation.descriptorPool = m_cullPool;
        setAllocation.descriptorSetCount = 1;
        setAllocation.pSetLayouts = &m_cullSetLayout;
        VkDescriptorSet set{};
        if (auto result = vkAllocateDescriptorSets(m_device, &setAllocation, &set);
            result != VK_SUCCESS) {
            return std::unexpected(cullError("vkAllocateDescriptorSets", result));
        }
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            writes[i] = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                set,
                i,
                0,
                1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr,
                &buffers[i],
                nullptr};
        }
        vkUpdateDescriptorSets(
            m_device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &m_cullSetLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        if (auto result = vkCreatePipelineLayout(m_device, &layout, nullptr, &m_cullLayout);
            result != VK_SUCCESS) {
            return std::unexpected(cullError("vkCreatePipelineLayout", result));
        }
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = shader.size_bytes();
        moduleInfo.pCode = shader.data();
        VkShaderModule module{};
        if (auto result = vkCreateShaderModule(m_device, &moduleInfo, nullptr, &module);
            result != VK_SUCCESS) {
            return std::unexpected(cullError("vkCreateShaderModule", result));
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
        pipeline.layout = m_cullLayout;
        const auto created =
            vkCreateComputePipelines(m_device, {}, 1, &pipeline, nullptr, &m_cullPipeline);
        vkDestroyShaderModule(m_device, module, nullptr);
        if (created != VK_SUCCESS) {
            return std::unexpected(cullError("vkCreateComputePipelines", created));
        }
        if (auto result = vkResetCommandPool(m_device, m_transferPool, 0); result != VK_SUCCESS) {
            return std::unexpected(cullError("vkResetCommandPool", result));
        }
        if (auto result = vkResetFences(m_device, 1, &m_transferFence); result != VK_SUCCESS) {
            return std::unexpected(cullError("vkResetFences", result));
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (auto result = vkBeginCommandBuffer(m_transferCommands, &begin); result != VK_SUCCESS) {
            return std::unexpected(cullError("vkBeginCommandBuffer", result));
        }
        vkCmdFillBuffer(m_transferCommands, buffers[4].buffer, 0, 8, 0);
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        if (groups != 0) {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            vkCmdPipelineBarrier2(m_transferCommands, &dependency);
            const std::array config{objectCount, static_cast<std::uint32_t>(ids.size())};
            vkCmdBindPipeline(m_transferCommands, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullPipeline);
            vkCmdBindDescriptorSets(
                m_transferCommands,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                m_cullLayout,
                0,
                1,
                &set,
                0,
                nullptr);
            vkCmdPushConstants(
                m_transferCommands, m_cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, config.data());
            vkCmdDispatch(m_transferCommands, groups, 1, 1);
        }
        barrier.srcStageMask =
            VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask =
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        vkCmdPipelineBarrier2(m_transferCommands, &dependency);
        if (auto result = vkEndCommandBuffer(m_transferCommands); result != VK_SUCCESS) {
            return std::unexpected(cullError("vkEndCommandBuffer", result));
        }
        VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        command.commandBuffer = m_transferCommands;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command;
        const auto submitted = vkQueueSubmit2(m_queue, 1, &submit, m_transferFence);
        m_pendingTransfer = submitted == VK_SUCCESS || submitted == VK_ERROR_DEVICE_LOST;
        if (submitted != VK_SUCCESS) {
            return std::unexpected(cullError("vkQueueSubmit2", submitted));
        }
        if (auto done = settleTransfer(true); !done) {
            return std::unexpected(done.error());
        }
        std::array<std::uint32_t, 2> counts{};
        if (auto read = readAllocation(
                m_cullBuffers[4].allocation, 0, std::as_writable_bytes(std::span{counts}));
            !read) {
            return std::unexpected(read.error());
        }
        if (counts[1] != std::min<std::size_t>(counts[0], ids.size()) || counts[0] > objectCount) {
            return invalid("GPU culling returned invalid counts");
        }
        if (counts[1] != 0) {
            if (auto read = readAllocation(
                    m_cullBuffers[2].allocation, 0, std::as_writable_bytes(ids.first(counts[1])));
                !read) {
                return std::unexpected(read.error());
            }
            if (auto read = readAllocation(
                    m_cullBuffers[3].allocation,
                    0,
                    std::as_writable_bytes(commands.first(counts[1])));
                !read) {
                return std::unexpected(read.error());
            }
        }
        return counts;
    };
    auto result = run();
    if (!m_pendingTransfer) {
        clearCullResources();
    }
    return result;
}

} // namespace ar::rhi::detail
