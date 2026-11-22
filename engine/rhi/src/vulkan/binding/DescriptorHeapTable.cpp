#include "binding/DescriptorHeapTable.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <string>

#include "VulkanResult.h"
#include "resources/DeviceResources.h"

namespace ar::rhi::detail {
namespace {

core::Error invalidArg(const char* message) {
    return {
        core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation, message, std::nullopt};
}

core::Error heapFailed(const char* operation, VkResult result) {
    return {
        core::ErrorCategory::InitializationFailed,
        core::ErrorStage::Capability,
        std::string(operation) + " returned " + vkResultName(result),
        core::ErrorOrigin{"vulkan", result}};
}

core::Error layoutError(DescriptorHeapLayoutError code) {
    const char* message = "Descriptor Heap layout failed";
    switch (code) {
    case DescriptorHeapLayoutError::InvalidLimits:
        message = "Descriptor Heap limits are invalid";
        break;
    case DescriptorHeapLayoutError::InvalidCapacity:
        message = "Descriptor Heap requires positive image and sampler capacities";
        break;
    case DescriptorHeapLayoutError::Overflow:
        message = "Descriptor Heap layout size overflowed";
        break;
    case DescriptorHeapLayoutError::ResourceHeapExceeded:
        message = "requested Resource Heap exceeds device maxResourceHeapSize";
        break;
    case DescriptorHeapLayoutError::SamplerHeapExceeded:
        message = "requested Sampler Heap exceeds device maxSamplerHeapSize";
        break;
    }
    return invalidArg(message);
}

} // namespace

DescriptorHeapTable::~DescriptorHeapTable() {
    close();
}

void DescriptorHeapTable::close() noexcept {
    if (m_resources && m_resources->m_allocator) {
        if (m_samplerBuffer) {
            vmaDestroyBuffer(m_resources->m_allocator, m_samplerBuffer, m_samplerAllocation);
        }
        if (m_resourceBuffer) {
            vmaDestroyBuffer(m_resources->m_allocator, m_resourceBuffer, m_resourceAllocation);
        }
    }
    m_resourceBuffer = VK_NULL_HANDLE;
    m_resourceAllocation = nullptr;
    m_resourceMapped = nullptr;
    m_resourceAddress = 0;
    m_samplerBuffer = VK_NULL_HANDLE;
    m_samplerAllocation = nullptr;
    m_samplerMapped = nullptr;
    m_samplerAddress = 0;
    m_device = VK_NULL_HANDLE;
    m_resources = nullptr;
}

std::expected<void, core::Error> DescriptorHeapTable::allocateHeap(
    VkDeviceSize size,
    VkDeviceSize alignment,
    PFN_vkGetBufferDeviceAddress getDeviceAddress,
    const char* label,
    VkBuffer& outBuffer,
    VmaAllocation_T*& outAllocation,
    std::byte*& outMapped,
    VkDeviceAddress& outAddress) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage =
        VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo resultInfo{};
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    const VkResult created = vmaCreateBufferWithAlignment(
        m_resources->m_allocator,
        &bufferInfo,
        &allocInfo,
        alignment,
        &buffer,
        &allocation,
        &resultInfo);
    if (created != VK_SUCCESS) {
        return std::unexpected(heapFailed(label, created));
    }
    if (!resultInfo.pMappedData) {
        vmaDestroyBuffer(m_resources->m_allocator, buffer, allocation);
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InitializationFailed,
                core::ErrorStage::Capability,
                std::string(label) + " allocation is not host mapped",
                std::nullopt});
    }

    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = buffer;
    const VkDeviceAddress address = getDeviceAddress(m_device, &addressInfo);
    if (address == 0 || (address % alignment) != 0) {
        vmaDestroyBuffer(m_resources->m_allocator, buffer, allocation);
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InitializationFailed,
                core::ErrorStage::Capability,
                std::string(label) + " device address does not satisfy heap alignment",
                std::nullopt});
    }

    outBuffer = buffer;
    outAllocation = allocation;
    outMapped = static_cast<std::byte*>(resultInfo.pMappedData);
    outAddress = address;
    return {};
}

std::expected<void, core::Error> DescriptorHeapTable::open(
    VkPhysicalDevice physical,
    VkDevice device,
    DeviceResources& resources,
    const DescriptorHeapLimits& limits,
    const DescriptorHeapDispatch& dispatch,
    DescriptorHeapCapacity capacity) {
    close();
    if (!device || !dispatch.writeResourceDescriptors || !dispatch.writeSamplerDescriptors ||
        !dispatch.cmdBindResourceHeap || !dispatch.cmdBindSamplerHeap || !dispatch.cmdPushData) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::LoaderSymbolMissing,
                core::ErrorStage::Capability,
                "Descriptor Heap device entry points are unavailable",
                std::nullopt});
    }

    const auto planned = planDescriptorHeaps(limits, capacity);
    if (!planned) {
        return std::unexpected(layoutError(planned.error()));
    }

    if (vkGetPhysicalDeviceDescriptorSizeEXT && physical) {
        const VkDeviceSize imageSize =
            vkGetPhysicalDeviceDescriptorSizeEXT(physical, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        const VkDeviceSize bufferSize =
            vkGetPhysicalDeviceDescriptorSizeEXT(physical, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        const VkDeviceSize samplerSize =
            vkGetPhysicalDeviceDescriptorSizeEXT(physical, VK_DESCRIPTOR_TYPE_SAMPLER);
        if (imageSize == 0 || imageSize > planned->imageStride ||
            (capacity.bufferCount > 0 && (bufferSize == 0 || bufferSize > planned->bufferStride)) ||
            samplerSize == 0 || samplerSize > planned->samplerStride) {
            return std::unexpected(
                core::Error{
                    core::ErrorCategory::QueryFailed,
                    core::ErrorStage::Capability,
                    "Descriptor Heap descriptor size exceeds planned stride",
                    std::nullopt});
        }
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

    if (auto ready = resources.ensureAllocator(); !ready) {
        return std::unexpected(std::move(ready.error()));
    }

    m_device = device;
    m_resources = &resources;
    m_limits = limits;
    m_dispatch = dispatch;
    m_capacity = capacity;
    m_plan = *planned;

    if (auto allocated = allocateHeap(
            m_plan.resourceHeapSize,
            m_limits.resourceHeapAlignment,
            getDeviceAddress,
            "vmaCreateBufferWithAlignment(resourceHeap)",
            m_resourceBuffer,
            m_resourceAllocation,
            m_resourceMapped,
            m_resourceAddress);
        !allocated) {
        close();
        return allocated;
    }

    if (auto allocated = allocateHeap(
            m_plan.samplerHeapSize,
            m_limits.samplerHeapAlignment,
            getDeviceAddress,
            "vmaCreateBufferWithAlignment(samplerHeap)",
            m_samplerBuffer,
            m_samplerAllocation,
            m_samplerMapped,
            m_samplerAddress);
        !allocated) {
        close();
        return allocated;
    }
    return {};
}

std::expected<void, core::Error>
DescriptorHeapTable::writeImageSlot(std::uint32_t slot, VkImage image, VkFormat format) {
    if (!m_resourceMapped || slot >= m_capacity.imageCount || image == VK_NULL_HANDLE) {
        return std::unexpected(invalidArg("invalid Descriptor Heap image slot or image"));
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageDescriptorInfoEXT imageDesc{VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
    imageDesc.pView = &viewInfo;
    imageDesc.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkResourceDescriptorInfoEXT resourceInfo{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    resourceInfo.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    resourceInfo.data.pImage = &imageDesc;

    const VkDeviceSize offset = static_cast<VkDeviceSize>(slot) * m_plan.imageStride;
    const VkHostAddressRangeEXT dst{
        m_resourceMapped + offset, static_cast<std::size_t>(m_plan.imageStride)};
    const VkResult written = m_dispatch.writeResourceDescriptors(m_device, 1, &resourceInfo, &dst);
    if (written != VK_SUCCESS) {
        return std::unexpected(heapFailed("vkWriteResourceDescriptorsEXT(sampledImage)", written));
    }

    const VkResult flushed = vmaFlushAllocation(
        m_resources->m_allocator, m_resourceAllocation, offset, m_plan.imageStride);
    if (flushed != VK_SUCCESS) {
        return std::unexpected(heapFailed("vmaFlushAllocation(resourceHeap)", flushed));
    }
    return {};
}

std::expected<void, core::Error> DescriptorHeapTable::writeBufferSlot(
    std::uint32_t slot, VkDeviceAddress address, VkDeviceSize size) {
    if (!m_resourceMapped || slot >= m_capacity.bufferCount || address == 0 || size == 0) {
        return std::unexpected(invalidArg("invalid Descriptor Heap buffer slot or address range"));
    }

    const VkDeviceAddressRangeEXT range{address, size};
    VkResourceDescriptorInfoEXT resourceInfo{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    resourceInfo.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resourceInfo.data.pAddressRange = &range;

    const VkDeviceSize offset =
        m_plan.bufferBaseOffset + static_cast<VkDeviceSize>(slot) * m_plan.bufferStride;
    const VkHostAddressRangeEXT dst{
        m_resourceMapped + offset, static_cast<std::size_t>(m_plan.bufferStride)};
    const VkResult written = m_dispatch.writeResourceDescriptors(m_device, 1, &resourceInfo, &dst);
    if (written != VK_SUCCESS) {
        return std::unexpected(heapFailed("vkWriteResourceDescriptorsEXT(storageBuffer)", written));
    }

    const VkResult flushed = vmaFlushAllocation(
        m_resources->m_allocator, m_resourceAllocation, offset, m_plan.bufferStride);
    if (flushed != VK_SUCCESS) {
        return std::unexpected(heapFailed("vmaFlushAllocation(resourceHeap)", flushed));
    }
    return {};
}

std::expected<void, core::Error>
DescriptorHeapTable::writeSamplerSlot(std::uint32_t slot, const VkSamplerCreateInfo& sampler) {
    if (!m_samplerMapped || slot >= m_capacity.samplerCount) {
        return std::unexpected(invalidArg("invalid Descriptor Heap sampler slot"));
    }

    const VkDeviceSize offset = static_cast<VkDeviceSize>(slot) * m_plan.samplerStride;
    const VkHostAddressRangeEXT dst{
        m_samplerMapped + offset, static_cast<std::size_t>(m_plan.samplerStride)};
    const VkResult written = m_dispatch.writeSamplerDescriptors(m_device, 1, &sampler, &dst);
    if (written != VK_SUCCESS) {
        return std::unexpected(heapFailed("vkWriteSamplerDescriptorsEXT", written));
    }

    const VkResult flushed = vmaFlushAllocation(
        m_resources->m_allocator, m_samplerAllocation, offset, m_plan.samplerStride);
    if (flushed != VK_SUCCESS) {
        return std::unexpected(heapFailed("vmaFlushAllocation(samplerHeap)", flushed));
    }
    return {};
}

VkBindHeapInfoEXT DescriptorHeapTable::resourceBindInfo() const noexcept {
    VkBindHeapInfoEXT info{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    info.heapRange = {m_resourceAddress, m_plan.resourceHeapSize};
    info.reservedRangeOffset = m_plan.resourceReservedOffset;
    info.reservedRangeSize = m_plan.resourceReservedSize;
    return info;
}

VkBindHeapInfoEXT DescriptorHeapTable::samplerBindInfo() const noexcept {
    VkBindHeapInfoEXT info{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    info.heapRange = {m_samplerAddress, m_plan.samplerHeapSize};
    info.reservedRangeOffset = m_plan.samplerReservedOffset;
    info.reservedRangeSize = m_plan.samplerReservedSize;
    return info;
}

} // namespace ar::rhi::detail
