#include "binding/DescriptorHeapProbe.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
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

std::expected<PFN_vkGetBufferDeviceAddress, core::Error> validateProbeEnvironment(
    VkPhysicalDevice physical,
    VkDevice device,
    std::uint32_t queueFamily,
    const DescriptorHeapLimits& limits,
    std::span<const std::uint32_t> computeShader) {
    if (computeShader.size() < 5 || computeShader.front() != kSpirvMagic) {
        return std::unexpected(invalidArg("Descriptor Heap probe requires valid SPIR-V bytecode"));
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
    return getDeviceAddress;
}

class HeapProbeRunner {
public:
    HeapProbeRunner(
        VkDevice device,
        VkQueue queue,
        DeviceResources& resources,
        const DescriptorHeapDispatch& dispatch) noexcept
        : m_device(device), m_queue(queue), m_resources(&resources), m_dispatch(dispatch) {}

    ~HeapProbeRunner() {
        if (m_inFlight) {
            m_table.abandon();
            return;
        }
        releasePipelineAndCloseTable();
        if (m_resources) {
            for (std::size_t i = m_ownedImages.size(); i > 0; --i) {
                static_cast<void>(m_resources->destroyImage(m_ownedImages[i - 1]));
            }
        }
    }

    HeapProbeRunner(const HeapProbeRunner&) = delete;
    HeapProbeRunner& operator=(const HeapProbeRunner&) = delete;

    [[nodiscard]] std::expected<ImageHandle, core::Error>
    createOwnedImage(ImageDesc desc, std::span<const std::byte> pixels) {
        auto created = m_resources->createSampledImage(desc, pixels);
        if (!created) {
            return std::unexpected(std::move(created.error()));
        }
        m_ownedImages.push_back(*created);
        return *created;
    }

    [[nodiscard]] std::expected<void, core::Error> destroyOwnedImage(ImageHandle handle) {
        auto destroyed = m_resources->destroyImage(handle);
        if (!destroyed) {
            return destroyed;
        }
        std::erase(m_ownedImages, handle);
        return {};
    }

    [[nodiscard]] std::expected<void, core::Error> openTable(
        VkPhysicalDevice physical,
        const DescriptorHeapLimits& limits,
        DescriptorHeapCapacity capacity,
        ImageHandle defaultImage,
        const VkSamplerCreateInfo& defaultSampler) {
        return m_table.open(
            physical,
            m_device,
            *m_resources,
            limits,
            m_dispatch,
            capacity,
            defaultImage,
            VK_FORMAT_R8G8B8A8_UNORM,
            defaultSampler);
    }

    [[nodiscard]] std::expected<void, core::Error> prepareExecution(
        std::uint32_t queueFamily,
        PFN_vkGetBufferDeviceAddress getDeviceAddress,
        std::span<const std::uint32_t> computeShader,
        std::size_t maxQueries) {
        m_outputCapacity = maxQueries;
        const VkDeviceSize outputBytes =
            static_cast<VkDeviceSize>(maxQueries) * sizeof(std::array<float, 4>);

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
                m_resources->allocator(),
                &outputInfo,
                &outputAlloc,
                &m_outputBuffer,
                &m_outputAllocation,
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
        m_outputMapped = mappedInfo.pMappedData;

        VkBufferDeviceAddressInfo outputAddressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        outputAddressInfo.buffer = m_outputBuffer;
        const VkDeviceAddress outputAddress = getDeviceAddress(m_device, &outputAddressInfo);
        if (outputAddress == 0) {
            return std::unexpected(
                core::Error{
                    core::ErrorCategory::InitializationFailed,
                    core::ErrorStage::Capability,
                    "Descriptor Heap probe output buffer has no device address",
                    std::nullopt});
        }
        if (auto written = m_table.writeBufferSlot(0, outputAddress, outputBytes); !written) {
            return std::unexpected(std::move(written.error()));
        }

        const auto& plan = m_table.plan();
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
                vkCreateShaderModule(m_device, &moduleInfo, nullptr, &m_shaderModule);
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

        VkPipelineCreateFlags2CreateInfo flags2{
            VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};
        flags2.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.pNext = &flags2;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.pNext = &mappingInfo;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = m_shaderModule;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = VK_NULL_HANDLE;

        const VkResult pipelineResult = vkCreateComputePipelines(
            m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
        vkDestroyShaderModule(m_device, m_shaderModule, nullptr);
        m_shaderModule = VK_NULL_HANDLE;
        if (pipelineResult != VK_SUCCESS) {
            return std::unexpected(
                probeFailed("vkCreateComputePipelines(heapProbe)", pipelineResult));
        }

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        if (const VkResult result = vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_pool);
            result != VK_SUCCESS) {
            return std::unexpected(probeFailed("vkCreateCommandPool(heapProbe)", result));
        }

        VkCommandBufferAllocateInfo allocCmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocCmd.commandPool = m_pool;
        allocCmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocCmd.commandBufferCount = 1;
        if (const VkResult result = vkAllocateCommandBuffers(m_device, &allocCmd, &m_commands);
            result != VK_SUCCESS) {
            return std::unexpected(probeFailed("vkAllocateCommandBuffers(heapProbe)", result));
        }

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (const VkResult result = vkCreateFence(m_device, &fenceInfo, nullptr, &m_fence);
            result != VK_SUCCESS) {
            return std::unexpected(probeFailed("vkCreateFence(heapProbe)", result));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, core::Error> submitQueries(
        std::span<const DescriptorHeapSampleQuery> queries,
        std::span<const VkImage> preComputeImages) {
        if (m_inFlight || queries.empty() || queries.size() > m_outputCapacity) {
            return std::unexpected(invalidArg("invalid Descriptor Heap probe submission state"));
        }
        if (m_fenceUsed) {
            if (const VkResult result = vkResetFences(m_device, 1, &m_fence);
                result != VK_SUCCESS) {
                return std::unexpected(probeFailed("vkResetFences(heapProbe)", result));
            }
            if (const VkResult result = vkResetCommandBuffer(m_commands, 0); result != VK_SUCCESS) {
                return std::unexpected(probeFailed("vkResetCommandBuffer(heapProbe)", result));
            }
        }

        const VkDeviceSize outputBytes =
            static_cast<VkDeviceSize>(queries.size()) * sizeof(std::array<float, 4>);
        std::memset(m_outputMapped, 0, static_cast<std::size_t>(outputBytes));
        if (const VkResult flushed =
                vmaFlushAllocation(m_resources->allocator(), m_outputAllocation, 0, outputBytes);
            flushed != VK_SUCCESS) {
            return std::unexpected(probeFailed("vmaFlushAllocation(heapProbeOutput)", flushed));
        }

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (const VkResult result = vkBeginCommandBuffer(m_commands, &beginInfo);
            result != VK_SUCCESS) {
            return std::unexpected(probeFailed("vkBeginCommandBuffer(heapProbe)", result));
        }

        if (!preComputeImages.empty()) {
            std::vector<VkImageMemoryBarrier2> imageBarriers(preComputeImages.size());
            for (std::size_t i = 0; i < preComputeImages.size(); ++i) {
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
                barrier.image = preComputeImages[i];
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            VkDependencyInfo preCompute{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            preCompute.imageMemoryBarrierCount = static_cast<std::uint32_t>(imageBarriers.size());
            preCompute.pImageMemoryBarriers = imageBarriers.data();
            vkCmdPipelineBarrier2(m_commands, &preCompute);
        }

        const VkBindHeapInfoEXT resourceBind = m_table.resourceBindInfo();
        const VkBindHeapInfoEXT samplerBind = m_table.samplerBindInfo();
        m_dispatch.cmdBindResourceHeap(m_commands, &resourceBind);
        m_dispatch.cmdBindSamplerHeap(m_commands, &samplerBind);
        vkCmdBindPipeline(m_commands, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);

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
            m_dispatch.cmdPushData(m_commands, &pushInfo);
            vkCmdDispatch(m_commands, 1, 1, 1);
        }

        VkBufferMemoryBarrier2 postCompute{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        postCompute.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        postCompute.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        postCompute.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        postCompute.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        postCompute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postCompute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postCompute.buffer = m_outputBuffer;
        postCompute.size = outputBytes;
        VkDependencyInfo hostVisible{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        hostVisible.bufferMemoryBarrierCount = 1;
        hostVisible.pBufferMemoryBarriers = &postCompute;
        vkCmdPipelineBarrier2(m_commands, &hostVisible);

        if (const VkResult result = vkEndCommandBuffer(m_commands); result != VK_SUCCESS) {
            return std::unexpected(probeFailed("vkEndCommandBuffer(heapProbe)", result));
        }

        VkCommandBufferSubmitInfo cmdSubmit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cmdSubmit.commandBuffer = m_commands;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdSubmit;
        if (const VkResult result = vkQueueSubmit2(m_queue, 1, &submit, m_fence);
            result != VK_SUCCESS) {
            if (result == VK_ERROR_DEVICE_LOST) {
                m_resources->markExternalLost();
            }
            return std::unexpected(probeFailed("vkQueueSubmit2(heapProbe)", result));
        }
        m_inFlight = true;
        m_fenceUsed = true;
        m_lastQueryCount = queries.size();
        return {};
    }

    [[nodiscard]] std::expected<std::vector<std::array<float, 4>>, core::Error>
    waitAndReadSamples() {
        const VkResult waited = vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, kProbeTimeoutNs);
        if (waited == VK_SUCCESS || waited == VK_ERROR_DEVICE_LOST) {
            m_inFlight = false;
            if (waited == VK_ERROR_DEVICE_LOST) {
                m_resources->markExternalLost();
            }
        } else {
            m_resources->markExternalPending();
        }
        if (waited != VK_SUCCESS) {
            return std::unexpected(probeFailed("vkWaitForFences(heapProbe)", waited));
        }

        const VkDeviceSize outputBytes =
            static_cast<VkDeviceSize>(m_lastQueryCount) * sizeof(std::array<float, 4>);
        if (const VkResult invalidated = vmaInvalidateAllocation(
                m_resources->allocator(), m_outputAllocation, 0, outputBytes);
            invalidated != VK_SUCCESS) {
            return std::unexpected(
                probeFailed("vmaInvalidateAllocation(heapProbeOutput)", invalidated));
        }

        std::vector<std::array<float, 4>> samples(m_lastQueryCount);
        std::memcpy(samples.data(), m_outputMapped, static_cast<std::size_t>(outputBytes));
        return samples;
    }

    void releasePipelineAndCloseTable() noexcept {
        if (m_fence) {
            vkDestroyFence(m_device, m_fence, nullptr);
            m_fence = VK_NULL_HANDLE;
        }
        if (m_pool) {
            vkDestroyCommandPool(m_device, m_pool, nullptr);
            m_pool = VK_NULL_HANDLE;
            m_commands = VK_NULL_HANDLE;
        }
        if (m_pipeline) {
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
            m_pipeline = VK_NULL_HANDLE;
        }
        if (m_shaderModule) {
            vkDestroyShaderModule(m_device, m_shaderModule, nullptr);
            m_shaderModule = VK_NULL_HANDLE;
        }
        if (m_outputBuffer && m_resources && m_resources->allocator()) {
            vmaDestroyBuffer(m_resources->allocator(), m_outputBuffer, m_outputAllocation);
            m_outputBuffer = VK_NULL_HANDLE;
            m_outputAllocation = nullptr;
            m_outputMapped = nullptr;
        }
        m_table.close();
    }

    DescriptorHeapTable& table() noexcept {
        return m_table;
    }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    DeviceResources* m_resources = nullptr;
    DescriptorHeapDispatch m_dispatch{};
    VkFence m_fence = VK_NULL_HANDLE;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_commands = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkShaderModule m_shaderModule = VK_NULL_HANDLE;
    VkBuffer m_outputBuffer = VK_NULL_HANDLE;
    VmaAllocation m_outputAllocation = nullptr;
    void* m_outputMapped = nullptr;
    std::size_t m_outputCapacity = 0;
    std::size_t m_lastQueryCount = 0;
    bool m_inFlight = false;
    bool m_fenceUsed = false;
    DescriptorHeapTable m_table;
    std::vector<ImageHandle> m_ownedImages;
};

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
    auto getDeviceAddress =
        validateProbeEnvironment(physical, device, queueFamily, limits, computeShader);
    if (!getDeviceAddress) {
        return std::unexpected(std::move(getDeviceAddress.error()));
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

    HeapProbeRunner runner(device, queue, resources, dispatch);

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

    auto whiteImage = runner.createOwnedImage({1, 1}, white);
    if (!whiteImage) {
        return std::unexpected(std::move(whiteImage.error()));
    }
    auto redBlueImage = runner.createOwnedImage({2, 1}, redBlue);
    if (!redBlueImage) {
        return std::unexpected(std::move(redBlueImage.error()));
    }
    auto greenImage = runner.createOwnedImage({1, 1}, green);
    if (!greenImage) {
        return std::unexpected(std::move(greenImage.error()));
    }

    const VkSamplerCreateInfo nearest = makeSamplerInfo(VK_FILTER_NEAREST);
    const VkSamplerCreateInfo linear = makeSamplerInfo(VK_FILTER_LINEAR);
    if (auto opened = runner.openTable(
            physical,
            limits,
            DescriptorHeapCapacity{kSetImageCount, 1, kSetSamplerCount},
            *whiteImage,
            nearest);
        !opened) {
        return std::unexpected(std::move(opened.error()));
    }
    if (auto reg = runner.table().registerImage(*redBlueImage); !reg) {
        return std::unexpected(std::move(reg.error()));
    }
    if (auto reg = runner.table().registerImage(*greenImage); !reg) {
        return std::unexpected(std::move(reg.error()));
    }
    if (auto reg = runner.table().registerSampler(linear); !reg) {
        return std::unexpected(std::move(reg.error()));
    }
    if (auto published = runner.table().publishCompleted(true); !published) {
        return std::unexpected(std::move(published.error()));
    }

    if (auto ready =
            runner.prepareExecution(queueFamily, *getDeviceAddress, computeShader, queries.size());
        !ready) {
        return std::unexpected(std::move(ready.error()));
    }

    const std::array preCompute{
        runner.table().imageForSlot(0),
        runner.table().imageForSlot(1),
        runner.table().imageForSlot(2)};
    if (auto submitted = runner.submitQueries(queries, preCompute); !submitted) {
        return std::unexpected(std::move(submitted.error()));
    }
    auto samples = runner.waitAndReadSamples();
    if (!samples) {
        return std::unexpected(std::move(samples.error()));
    }

    DescriptorHeapProbeResult result;
    result.resourceHeapAddress = runner.table().resourceHeapAddress();
    result.samplerHeapAddress = runner.table().samplerHeapAddress();
    result.samples = std::move(*samples);
    return result;
}

std::expected<DescriptorHeapRetirementResult, core::Error> runDescriptorHeapRetirementProbe(
    VkPhysicalDevice physical,
    VkDevice device,
    VkQueue queue,
    std::uint32_t queueFamily,
    DeviceResources& resources,
    const DescriptorHeapLimits& limits,
    const DescriptorHeapDispatch& dispatch,
    std::span<const std::uint32_t> computeShader) {
    auto getDeviceAddress =
        validateProbeEnvironment(physical, device, queueFamily, limits, computeShader);
    if (!getDeviceAddress) {
        return std::unexpected(std::move(getDeviceAddress.error()));
    }

    HeapProbeRunner runner(device, queue, resources, dispatch);
    DescriptorHeapRetirementResult result{};

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
    constexpr std::array yellow{std::byte{255}, std::byte{255}, std::byte{0}, std::byte{255}};
    constexpr std::array green{std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}};
    constexpr std::array magenta{std::byte{255}, std::byte{0}, std::byte{255}, std::byte{255}};
    constexpr std::array cyan{std::byte{0}, std::byte{255}, std::byte{255}, std::byte{255}};

    auto whiteImage = runner.createOwnedImage({1, 1}, white);
    if (!whiteImage) {
        return std::unexpected(std::move(whiteImage.error()));
    }
    auto redBlueImage = runner.createOwnedImage({2, 1}, redBlue);
    if (!redBlueImage) {
        return std::unexpected(std::move(redBlueImage.error()));
    }
    auto pendingImage = runner.createOwnedImage({1, 1}, yellow);
    if (!pendingImage) {
        return std::unexpected(std::move(pendingImage.error()));
    }
    auto greenImage = runner.createOwnedImage({1, 1}, green);
    if (!greenImage) {
        return std::unexpected(std::move(greenImage.error()));
    }
    auto magentaImage = runner.createOwnedImage({1, 1}, magenta);
    if (!magentaImage) {
        return std::unexpected(std::move(magentaImage.error()));
    }
    auto activeOnCloseImage = runner.createOwnedImage({1, 1}, cyan);
    if (!activeOnCloseImage) {
        return std::unexpected(std::move(activeOnCloseImage.error()));
    }

    const VkSamplerCreateInfo nearest = makeSamplerInfo(VK_FILTER_NEAREST);
    const VkSamplerCreateInfo linear = makeSamplerInfo(VK_FILTER_LINEAR);
    if (auto opened = runner.openTable(
            physical,
            limits,
            DescriptorHeapCapacity{kSetImageCount, 1, kSetSamplerCount},
            *whiteImage,
            nearest);
        !opened) {
        return std::unexpected(std::move(opened.error()));
    }

    auto redBlueBinding = runner.table().registerImage(*redBlueImage);
    if (!redBlueBinding) {
        return std::unexpected(std::move(redBlueBinding.error()));
    }

    // Verify retiring a Pending (unpublished) slot immediately releases its image reference and
    // frees the slot without waiting for GPU completion.
    auto pendingBinding = runner.table().registerImage(*pendingImage);
    if (!pendingBinding) {
        return std::unexpected(std::move(pendingBinding.error()));
    }
    const bool pendingProtected = !resources.destroyImage(*pendingImage).has_value();
    const bool pendingUnresolved = !runner.table().resolveImage(*pendingBinding).has_value();
    const bool pendingRetired = runner.table().retireImage(*pendingBinding);
    const auto destroyedPending = runner.destroyOwnedImage(*pendingImage);
    result.pendingImageDestroySucceeded =
        pendingProtected && pendingUnresolved && pendingRetired && destroyedPending.has_value();

    auto greenBinding = runner.table().registerImage(*greenImage);
    if (!greenBinding) {
        return std::unexpected(std::move(greenBinding.error()));
    }
    result.pendingRetireFreedImmediately = greenBinding->slot == pendingBinding->slot &&
                                           greenBinding->generation > pendingBinding->generation;

    // Fill all remaining image and sampler slots so retiring slots cannot be silently bypassed by
    // allocating a different free slot.
    auto activeOnCloseBinding = runner.table().registerImage(*activeOnCloseImage);
    if (!activeOnCloseBinding) {
        return std::unexpected(std::move(activeOnCloseBinding.error()));
    }
    for (std::uint32_t slot = 4; slot < kSetImageCount; ++slot) {
        if (auto filler = runner.table().registerImage(*whiteImage); !filler) {
            return std::unexpected(std::move(filler.error()));
        }
    }

    auto linearBinding = runner.table().registerSampler(linear);
    if (!linearBinding) {
        return std::unexpected(std::move(linearBinding.error()));
    }
    for (std::uint32_t slot = 2; slot < kSetSamplerCount; ++slot) {
        if (auto filler = runner.table().registerSampler(nearest); !filler) {
            return std::unexpected(std::move(filler.error()));
        }
    }

    if (auto published = runner.table().publishCompleted(true); !published) {
        return std::unexpected(std::move(published.error()));
    }
    const auto resolvedGreen = runner.table().resolveImage(*greenBinding);
    const auto resolvedLinear = runner.table().resolveSampler(*linearBinding);
    if (!resolvedGreen || !resolvedLinear) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "published Descriptor Heap slots failed to resolve",
                std::nullopt});
    }
    result.retiredImageSlot = *resolvedGreen;
    result.retiredImageGeneration = greenBinding->generation;
    result.retiredSamplerSlot = *resolvedLinear;
    result.retiredSamplerGeneration = linearBinding->generation;

    if (auto ready = runner.prepareExecution(queueFamily, *getDeviceAddress, computeShader, 2);
        !ready) {
        return std::unexpected(std::move(ready.error()));
    }

    // Stage 1: submit GPU work reading the target image & sampler slots, then retire both handles
    // BEFORE waiting on the fence.
    const std::array stageQueries{
        DescriptorHeapSampleQuery{result.retiredImageSlot, 0, 0.5F, 0.5F},
        DescriptorHeapSampleQuery{1, result.retiredSamplerSlot, 0.45F, 0.5F},
    };
    const std::array stage1Barriers{
        runner.table().imageForSlot(0),
        runner.table().imageForSlot(1),
        runner.table().imageForSlot(result.retiredImageSlot)};
    if (auto submitted = runner.submitQueries(stageQueries, stage1Barriers); !submitted) {
        return std::unexpected(std::move(submitted.error()));
    }

    // Do not return early while GPU work is in flight; record all checks, then wait the fence.
    const bool retiredImage = runner.table().retireImage(*greenBinding);
    result.staleImageHandleRejectedOnRetire =
        retiredImage && !runner.table().resolveImage(*greenBinding).has_value();
    result.duplicateImageRetireRejected = !runner.table().retireImage(*greenBinding);

    const bool retiredSampler = runner.table().retireSampler(*linearBinding);
    result.staleSamplerHandleRejectedOnRetire =
        retiredSampler && !runner.table().resolveSampler(*linearBinding).has_value();
    result.duplicateSamplerRetireRejected = !runner.table().retireSampler(*linearBinding);

    const auto deferred = runner.table().publishCompleted(false);
    result.incompletePublishDeferred = deferred.has_value() && !*deferred;

    const auto destroyWhileInFlight = resources.destroyImage(*greenImage);
    result.inFlightDestroyRejected =
        !destroyWhileInFlight.has_value() &&
        destroyWhileInFlight.error().category == core::ErrorCategory::InvalidState;

    const auto uploadWhileInFlight = resources.uploadImage(*greenImage, yellow);
    result.inFlightUploadRejected =
        !uploadWhileInFlight.has_value() &&
        uploadWhileInFlight.error().category == core::ErrorCategory::InvalidState;

    std::array<std::byte, 4> readbackBytes{};
    const auto readbackWhileInFlight = resources.readbackImage(*greenImage, readbackBytes);
    result.inFlightReadbackRejected =
        !readbackWhileInFlight.has_value() &&
        readbackWhileInFlight.error().category == core::ErrorCategory::InvalidState;

    const auto reuseImageWhileInFlight = runner.table().registerImage(*magentaImage);
    result.inFlightReuseBlocked =
        !reuseImageWhileInFlight.has_value() &&
        reuseImageWhileInFlight.error().category == core::ErrorCategory::InvalidState;

    const auto reuseSamplerWhileInFlight = runner.table().registerSampler(linear);
    result.inFlightSamplerReuseBlocked =
        !reuseSamplerWhileInFlight.has_value() &&
        reuseSamplerWhileInFlight.error().category == core::ErrorCategory::InvalidState;

    auto stage1Samples = runner.waitAndReadSamples();
    if (!stage1Samples) {
        return std::unexpected(std::move(stage1Samples.error()));
    }
    result.inFlightRetiredImageSample = (*stage1Samples)[0];
    result.inFlightRetiredSamplerSample = (*stage1Samples)[1];

    // Fence completed: publish retirement so Heap slots are overwritten with slot 0 defaults and
    // the underlying green image reference is released.
    auto applied = runner.table().publishCompleted(true);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    result.completedPublishApplied = *applied;

    // Verify underlying green image is now open for readback, upload, and destruction.
    if (auto rb = resources.readbackImage(*greenImage, readbackBytes); rb.has_value()) {
        result.postCompletionReadbackSucceeded = (readbackBytes == green);
    }
    if (auto up = resources.uploadImage(*greenImage, yellow); up.has_value()) {
        result.postCompletionUploadSucceeded = resources.waitTransfer(*up).has_value();
    }
    result.postCompletionDestroySucceeded = runner.destroyOwnedImage(*greenImage).has_value();

    // Stage 2: dispatch on the exact same slot indices BEFORE reuse to confirm Heap memory now
    // holds slot 0 defaults (white image, nearest sampler).
    if (auto submitted = runner.submitQueries(stageQueries, {}); !submitted) {
        return std::unexpected(std::move(submitted.error()));
    }
    auto stage2Samples = runner.waitAndReadSamples();
    if (!stage2Samples) {
        return std::unexpected(std::move(stage2Samples.error()));
    }
    result.postRetirementDefaultImageSample = (*stage2Samples)[0];
    result.postRetirementDefaultSamplerSample = (*stage2Samples)[1];

    // Stage 3: reuse both slots with incremented generations and publish new descriptors.
    auto reusedImageBinding = runner.table().registerImage(*magentaImage);
    if (!reusedImageBinding) {
        return std::unexpected(std::move(reusedImageBinding.error()));
    }
    auto reusedSamplerBinding = runner.table().registerSampler(linear);
    if (!reusedSamplerBinding) {
        return std::unexpected(std::move(reusedSamplerBinding.error()));
    }
    const bool pendingBeforeRepublish =
        !runner.table().resolveImage(*reusedImageBinding).has_value() &&
        !runner.table().resolveSampler(*reusedSamplerBinding).has_value();
    result.staleHandleRejectedAfterReuse =
        pendingBeforeRepublish && !runner.table().resolveImage(*greenBinding).has_value() &&
        !runner.table().retireImage(*greenBinding) &&
        !runner.table().resolveSampler(*linearBinding).has_value() &&
        !runner.table().retireSampler(*linearBinding);

    if (auto republished = runner.table().publishCompleted(true); !republished) {
        return std::unexpected(std::move(republished.error()));
    }
    const auto resolvedReusedImage = runner.table().resolveImage(*reusedImageBinding);
    const auto resolvedReusedSampler = runner.table().resolveSampler(*reusedSamplerBinding);
    if (!resolvedReusedImage || !resolvedReusedSampler) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "reused Descriptor Heap slots failed to resolve",
                std::nullopt});
    }
    result.reusedImageSlot = *resolvedReusedImage;
    result.reusedImageGeneration = reusedImageBinding->generation;
    result.reusedSamplerSlot = *resolvedReusedSampler;
    result.reusedSamplerGeneration = reusedSamplerBinding->generation;

    const std::array stage3Barriers{runner.table().imageForSlot(result.reusedImageSlot)};
    if (auto submitted = runner.submitQueries(stageQueries, stage3Barriers); !submitted) {
        return std::unexpected(std::move(submitted.error()));
    }
    auto stage3Samples = runner.waitAndReadSamples();
    if (!stage3Samples) {
        return std::unexpected(std::move(stage3Samples.error()));
    }
    result.reusedImageSample = (*stage3Samples)[0];
    result.reusedSamplerSample = (*stage3Samples)[1];

    // Ordered shutdown: active slot 3 protects activeOnCloseImage until pipeline + Heap table
    // close, after which destroying activeOnCloseImage must succeed.
    const bool blockedBeforeClose = !resources.destroyImage(*activeOnCloseImage).has_value();
    runner.releasePipelineAndCloseTable();
    result.orderedCloseReleasedActiveSlotImage =
        blockedBeforeClose && runner.destroyOwnedImage(*activeOnCloseImage).has_value();
    return result;
}

} // namespace ar::rhi::detail
