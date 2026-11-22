#include "binding/DescriptorHeapProbe.h"

#include <vk_mem_alloc.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "VulkanResult.h"
#include "binding/SetBindingLimits.h"
#include "resources/DeviceResources.h"

namespace ar::rhi::detail {
namespace {

constexpr std::uint32_t kSpirvMagic = 0x07230203u;
constexpr std::size_t kMaxProbeQueries = 64;
constexpr std::uint64_t kProbeTimeoutNs = 5'000'000'000ull;

struct HeapProbePushData {
    std::uint32_t imageSlot = 0;
    std::uint32_t samplerSlot = 0;
    std::uint32_t outputIndex = 0;
    std::uint32_t reserved = 0;
    float u = 0.F;
    float v = 0.F;
};
static_assert(sizeof(HeapProbePushData) == 24);

core::Error invalidArg(const char* message) {
    return {
        core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation, message, std::nullopt};
}

core::Error probeFailed(const char* operation, VkResult result) {
    return {
        core::ErrorCategory::InitializationFailed,
        core::ErrorStage::Capability,
        std::string(operation) + " returned " + vkResultName(result),
        core::ErrorOrigin{"vulkan", result}};
}

VkSamplerCreateInfo makeSamplerInfo(VkFilter filter) noexcept {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = filter;
    info.minFilter = filter;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = 1.0F;
    return info;
}

bool queueSupportsCompute(VkPhysicalDevice physical, std::uint32_t queueFamily) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    if (queueFamily >= count) {
        return false;
    }
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    return (families[queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
}

} // namespace

std::expected<DescriptorHeapProbeResult, core::Error> runDescriptorHeapProbe(
    VkPhysicalDevice physical,
    VkDevice device,
    VkQueue queue,
    std::uint32_t queueFamily,
    DeviceResources& resources,
    const DescriptorHeapLimits& limits,
    const DescriptorHeapDispatch& dispatch,
    std::span<const std::uint32_t> computeShader,
    std::span<const DescriptorHeapSampleQuery> queries) {
    if (computeShader.size() < 5 || computeShader.front() != kSpirvMagic) {
        return std::unexpected(invalidArg("Descriptor Heap probe requires valid SPIR-V bytecode"));
    }
    if (queries.empty() || queries.size() > kMaxProbeQueries) {
        return std::unexpected(
            invalidArg("Descriptor Heap probe requires between 1 and 64 sample queries"));
    }
    for (const auto& query : queries) {
        if (query.imageSlot >= kSetImageCount || query.samplerSlot >= kSetSamplerCount ||
            !std::isfinite(query.u) || !std::isfinite(query.v)) {
            return std::unexpected(invalidArg("Descriptor Heap probe query is out of range"));
        }
    }
    if (limits.maxPushDataSize < sizeof(HeapProbePushData)) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::QueryFailed,
                core::ErrorStage::Capability,
                "device maxPushDataSize is smaller than Descriptor Heap probe push data",
                std::nullopt});
    }
    if (!queueSupportsCompute(physical, queueFamily)) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::QueryFailed,
                core::ErrorStage::Capability,
                "graphics queue family does not support compute for Descriptor Heap probe",
                std::nullopt});
    }

    auto getDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
        vkGetDeviceProcAddr ? vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddress") : nullptr);
    if (!getDeviceAddress) {
        getDeviceAddress = vkGetBufferDeviceAddress;
    }
    if (!getDeviceAddress) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::LoaderSymbolMissing,
                core::ErrorStage::Capability,
                "vkGetBufferDeviceAddress entry point is unavailable",
                std::nullopt});
    }

    struct ProbeGuard {
        VkDevice device = VK_NULL_HANDLE;
        DeviceResources* resources = nullptr;
        VkFence fence = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        VkBuffer outputBuffer = VK_NULL_HANDLE;
        VmaAllocation outputAllocation = nullptr;
        DescriptorHeapTable table;
        std::array<ImageHandle, 3> images{};
        std::array<bool, 3> retained{};
        bool inFlight = false;

        ~ProbeGuard() {
            if (inFlight) {
                table.abandon();
                return;
            }
            if (fence) {
                vkDestroyFence(device, fence, nullptr);
            }
            if (pool) {
                vkDestroyCommandPool(device, pool, nullptr);
            }
            if (pipeline) {
                vkDestroyPipeline(device, pipeline, nullptr);
            }
            if (shaderModule) {
                vkDestroyShaderModule(device, shaderModule, nullptr);
            }
            if (outputBuffer && resources && resources->allocator()) {
                vmaDestroyBuffer(resources->allocator(), outputBuffer, outputAllocation);
            }
            table.close();
            if (resources) {
                for (std::size_t i = images.size(); i > 0; --i) {
                    const std::size_t index = i - 1;
                    if (retained[index]) {
                        resources->releaseSampledImage(images[index]);
                    }
                    if (images[index].generation != 0) {
                        static_cast<void>(resources->destroyImage(images[index]));
                    }
                }
            }
        }
    } guard{device, &resources};

    constexpr std::array white{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    constexpr std::array redBlue{
        std::byte{255},
        std::byte{0},
        std::byte{0},
        std::byte{255},
        std::byte{0},
        std::byte{0},
        std::byte{255},
        std::byte{255}};
    constexpr std::array green{std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}};

    const std::array<std::pair<ImageDesc, std::span<const std::byte>>, 3> inputs{{
        {{1, 1}, white},
        {{2, 1}, redBlue},
        {{1, 1}, green},
    }};
    std::array<VkImage, 3> vkImages{};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto created = resources.createSampledImage(inputs[i].first, inputs[i].second);
        if (!created) {
            return std::unexpected(std::move(created.error()));
        }
        guard.images[i] = *created;
        auto retained = resources.retainSampledImage(*created);
        if (!retained) {
            return std::unexpected(std::move(retained.error()));
        }
        guard.retained[i] = true;
        vkImages[i] = *retained;
    }

    if (auto opened = guard.table.open(
            physical,
            device,
            resources,
            limits,
            dispatch,
            DescriptorHeapCapacity{kSetImageCount, 1, kSetSamplerCount});
        !opened) {
        return std::unexpected(std::move(opened.error()));
    }

    for (std::uint32_t slot = 0; slot < kSetImageCount; ++slot) {
        const VkImage image = slot < vkImages.size() ? vkImages[slot] : vkImages[0];
        if (auto written = guard.table.writeImageSlot(slot, image, VK_FORMAT_R8G8B8A8_UNORM);
            !written) {
            return std::unexpected(std::move(written.error()));
        }
    }

    const VkSamplerCreateInfo nearest = makeSamplerInfo(VK_FILTER_NEAREST);
    const VkSamplerCreateInfo linear = makeSamplerInfo(VK_FILTER_LINEAR);
    for (std::uint32_t slot = 0; slot < kSetSamplerCount; ++slot) {
        const auto& sampler = slot == 1 ? linear : nearest;
        if (auto written = guard.table.writeSamplerSlot(slot, sampler); !written) {
            return std::unexpected(std::move(written.error()));
        }
    }

    const VkDeviceSize outputBytes =
        static_cast<VkDeviceSize>(queries.size()) * sizeof(std::array<float, 4>);
    VkBufferCreateInfo outputInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    outputInfo.size = outputBytes;
    outputInfo.usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    outputInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo outputAlloc{};
    outputAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    outputAlloc.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mappedInfo{};
    if (const VkResult result = vmaCreateBuffer(
            resources.allocator(),
            &outputInfo,
            &outputAlloc,
            &guard.outputBuffer,
            &guard.outputAllocation,
            &mappedInfo);
        result != VK_SUCCESS) {
        return std::unexpected(probeFailed("vmaCreateBuffer(heapProbeOutput)", result));
    }
    if (!mappedInfo.pMappedData) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InitializationFailed,
                core::ErrorStage::Capability,
                "Descriptor Heap probe output buffer is not host mapped",
                std::nullopt});
    }
    std::memset(mappedInfo.pMappedData, 0, static_cast<std::size_t>(outputBytes));
    if (const VkResult flushed =
            vmaFlushAllocation(resources.allocator(), guard.outputAllocation, 0, outputBytes);
        flushed != VK_SUCCESS) {
        return std::unexpected(probeFailed("vmaFlushAllocation(heapProbeOutput)", flushed));
    }

    VkBufferDeviceAddressInfo outputAddressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    outputAddressInfo.buffer = guard.outputBuffer;
    const VkDeviceAddress outputAddress = getDeviceAddress(device, &outputAddressInfo);
    if (outputAddress == 0) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InitializationFailed,
                core::ErrorStage::Capability,
                "Descriptor Heap probe output buffer has no device address",
                std::nullopt});
    }
    if (auto written = guard.table.writeBufferSlot(0, outputAddress, outputBytes); !written) {
        return std::unexpected(std::move(written.error()));
    }

    const auto& plan = guard.table.plan();
    if (plan.imageStride > (std::numeric_limits<std::uint32_t>::max)() ||
        plan.samplerStride > (std::numeric_limits<std::uint32_t>::max)() ||
        plan.bufferBaseOffset > (std::numeric_limits<std::uint32_t>::max)() ||
        plan.bufferStride > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::QueryFailed,
                core::ErrorStage::Capability,
                "Descriptor Heap offsets or strides exceed 32-bit mapping fields",
                std::nullopt});
    }

    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = computeShader.size_bytes();
    moduleInfo.pCode = computeShader.data();
    if (const VkResult result =
            vkCreateShaderModule(device, &moduleInfo, nullptr, &guard.shaderModule);
        result != VK_SUCCESS) {
        return std::unexpected(probeFailed("vkCreateShaderModule(heapProbe)", result));
    }

    VkDescriptorSetAndBindingMappingEXT imageMapping{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT};
    imageMapping.descriptorSet = 0;
    imageMapping.firstBinding = 0;
    imageMapping.bindingCount = 1;
    imageMapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
    imageMapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
    imageMapping.sourceData.constantOffset.heapOffset = 0;
    imageMapping.sourceData.constantOffset.heapArrayStride =
        static_cast<std::uint32_t>(plan.imageStride);

    VkDescriptorSetAndBindingMappingEXT samplerMapping{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT};
    samplerMapping.descriptorSet = 0;
    samplerMapping.firstBinding = 1;
    samplerMapping.bindingCount = 1;
    samplerMapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
    samplerMapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
    samplerMapping.sourceData.constantOffset.heapOffset = 0;
    samplerMapping.sourceData.constantOffset.heapArrayStride =
        static_cast<std::uint32_t>(plan.samplerStride);

    VkDescriptorSetAndBindingMappingEXT bufferMapping{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT};
    bufferMapping.descriptorSet = 0;
    bufferMapping.firstBinding = 2;
    bufferMapping.bindingCount = 1;
    bufferMapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT |
                                 VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT;
    bufferMapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
    bufferMapping.sourceData.constantOffset.heapOffset =
        static_cast<std::uint32_t>(plan.bufferBaseOffset);
    bufferMapping.sourceData.constantOffset.heapArrayStride =
        static_cast<std::uint32_t>(plan.bufferStride);

    const std::array mappings{imageMapping, samplerMapping, bufferMapping};
    VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{
        VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT};
    mappingInfo.mappingCount = static_cast<std::uint32_t>(mappings.size());
    mappingInfo.pMappings = mappings.data();

    VkPipelineCreateFlags2CreateInfo flags2{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};
    flags2.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &flags2;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.pNext = &mappingInfo;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = guard.shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = VK_NULL_HANDLE;

    const VkResult pipelineResult = vkCreateComputePipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &guard.pipeline);
    vkDestroyShaderModule(device, guard.shaderModule, nullptr);
    guard.shaderModule = VK_NULL_HANDLE;
    if (pipelineResult != VK_SUCCESS) {
        return std::unexpected(probeFailed("vkCreateComputePipelines(heapProbe)", pipelineResult));
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    if (const VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr, &guard.pool);
        result != VK_SUCCESS) {
        return std::unexpected(probeFailed("vkCreateCommandPool(heapProbe)", result));
    }

    VkCommandBufferAllocateInfo allocCmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocCmd.commandPool = guard.pool;
    allocCmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCmd.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (const VkResult result = vkAllocateCommandBuffers(device, &allocCmd, &commands);
        result != VK_SUCCESS) {
        return std::unexpected(probeFailed("vkAllocateCommandBuffers(heapProbe)", result));
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (const VkResult result = vkCreateFence(device, &fenceInfo, nullptr, &guard.fence);
        result != VK_SUCCESS) {
        return std::unexpected(probeFailed("vkCreateFence(heapProbe)", result));
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (const VkResult result = vkBeginCommandBuffer(commands, &beginInfo); result != VK_SUCCESS) {
        return std::unexpected(probeFailed("vkBeginCommandBuffer(heapProbe)", result));
    }

    std::array<VkImageMemoryBarrier2, 3> imageBarriers{};
    for (std::size_t i = 0; i < vkImages.size(); ++i) {
        auto& barrier = imageBarriers[i];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vkImages[i];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo preCompute{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    preCompute.imageMemoryBarrierCount = static_cast<std::uint32_t>(imageBarriers.size());
    preCompute.pImageMemoryBarriers = imageBarriers.data();
    vkCmdPipelineBarrier2(commands, &preCompute);

    const VkBindHeapInfoEXT resourceBind = guard.table.resourceBindInfo();
    const VkBindHeapInfoEXT samplerBind = guard.table.samplerBindInfo();
    dispatch.cmdBindResourceHeap(commands, &resourceBind);
    dispatch.cmdBindSamplerHeap(commands, &samplerBind);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, guard.pipeline);

    for (std::size_t index = 0; index < queries.size(); ++index) {
        const auto& query = queries[index];
        const HeapProbePushData push{
            query.imageSlot,
            query.samplerSlot,
            static_cast<std::uint32_t>(index),
            0,
            query.u,
            query.v};
        VkPushDataInfoEXT pushInfo{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
        pushInfo.offset = 0;
        pushInfo.data = {&push, sizeof(push)};
        dispatch.cmdPushData(commands, &pushInfo);
        vkCmdDispatch(commands, 1, 1, 1);
    }

    VkBufferMemoryBarrier2 postCompute{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    postCompute.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    postCompute.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    postCompute.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    postCompute.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    postCompute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postCompute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postCompute.buffer = guard.outputBuffer;
    postCompute.size = outputBytes;
    VkDependencyInfo hostVisible{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    hostVisible.bufferMemoryBarrierCount = 1;
    hostVisible.pBufferMemoryBarriers = &postCompute;
    vkCmdPipelineBarrier2(commands, &hostVisible);

    if (const VkResult result = vkEndCommandBuffer(commands); result != VK_SUCCESS) {
        return std::unexpected(probeFailed("vkEndCommandBuffer(heapProbe)", result));
    }

    VkCommandBufferSubmitInfo cmdSubmit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdSubmit.commandBuffer = commands;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdSubmit;
    if (const VkResult result = vkQueueSubmit2(queue, 1, &submit, guard.fence);
        result != VK_SUCCESS) {
        if (result == VK_ERROR_DEVICE_LOST) {
            resources.markExternalLost();
        }
        return std::unexpected(probeFailed("vkQueueSubmit2(heapProbe)", result));
    }
    guard.inFlight = true;
    const VkResult waited = vkWaitForFences(device, 1, &guard.fence, VK_TRUE, kProbeTimeoutNs);
    if (waited == VK_SUCCESS || waited == VK_ERROR_DEVICE_LOST) {
        guard.inFlight = false;
        if (waited == VK_ERROR_DEVICE_LOST) {
            resources.markExternalLost();
        }
    } else {
        resources.markExternalPending();
    }
    if (waited != VK_SUCCESS) {
        return std::unexpected(probeFailed("vkWaitForFences(heapProbe)", waited));
    }

    if (const VkResult invalidated =
            vmaInvalidateAllocation(resources.allocator(), guard.outputAllocation, 0, outputBytes);
        invalidated != VK_SUCCESS) {
        return std::unexpected(
            probeFailed("vmaInvalidateAllocation(heapProbeOutput)", invalidated));
    }

    DescriptorHeapProbeResult result;
    result.resourceHeapAddress = guard.table.resourceHeapAddress();
    result.samplerHeapAddress = guard.table.samplerHeapAddress();
    result.samples.resize(queries.size());
    std::memcpy(
        result.samples.data(), mappedInfo.pMappedData, static_cast<std::size_t>(outputBytes));
    return result;
}

} // namespace ar::rhi::detail
