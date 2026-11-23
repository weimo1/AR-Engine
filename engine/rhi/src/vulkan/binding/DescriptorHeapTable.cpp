#include "binding/DescriptorHeapTable.h"

#include <vk_mem_alloc.h>

#include <string>
#include <utility>

#include "VulkanResult.h"
#include "resources/DeviceResources.h"

namespace ar::rhi::detail {
namespace {

core::Error invalidArg(const char* message) {
    return {
        core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation, message, std::nullopt};
}

core::Error invalidState(const char* message) {
    return {core::ErrorCategory::InvalidState, core::ErrorStage::Capability, message, std::nullopt};
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
    if (m_imageSlots && m_resources) {
        m_imageSlots->clear([this](const HeapImageDescriptor& descriptor) noexcept {
            m_resources->releaseSampledImage(descriptor.handle);
        });
    }
    m_imageSlots.reset();
    m_samplerSlots.reset();
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
    DescriptorHeapCapacity capacity,
    ImageHandle defaultImage,
    VkFormat defaultImageFormat,
    const VkSamplerCreateInfo& defaultSampler) {
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
    if (capacity.imageCount > kStaticSceneImageLimit || capacity.samplerCount > kSetSamplerCount) {
        return std::unexpected(invalidArg("Descriptor Heap slot capacity exceeds table limit"));
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

    auto retainedDefault = resources.retainSampledImage(defaultImage);
    if (!retainedDefault) {
        return std::unexpected(std::move(retainedDefault.error()));
    }

    m_device = device;
    m_resources = &resources;
    m_limits = limits;
    m_dispatch = dispatch;
    m_capacity = capacity;
    m_plan = *planned;
    m_imageSlots.emplace(
        HeapImageDescriptor{defaultImage, *retainedDefault, defaultImageFormat},
        capacity.imageCount);
    m_samplerSlots.emplace(defaultSampler, capacity.samplerCount);

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

    for (std::uint32_t slot = 0; slot < m_capacity.imageCount; ++slot) {
        if (auto written = writeImageSlot(slot, *retainedDefault, defaultImageFormat); !written) {
            close();
            return written;
        }
    }
    for (std::uint32_t slot = 0; slot < m_capacity.samplerCount; ++slot) {
        if (auto written = writeSamplerSlot(slot, defaultSampler); !written) {
            close();
            return written;
        }
    }
    return {};
}

std::expected<DescriptorHeapTable::ImageBinding, core::Error>
DescriptorHeapTable::registerImage(ImageHandle image, VkFormat format) {
    if (!m_imageSlots || !m_resources) {
        return std::unexpected(invalidState("Descriptor Heap table is not open"));
    }
    auto retained = m_resources->retainSampledImage(image);
    if (!retained) {
        return std::unexpected(std::move(retained.error()));
    }
    auto handle = m_imageSlots->registerPending(HeapImageDescriptor{image, *retained, format});
    if (!handle) {
        m_resources->releaseSampledImage(image);
        return std::unexpected(invalidState("Descriptor Heap image slot capacity exhausted"));
    }
    return *handle;
}

std::optional<std::uint32_t> DescriptorHeapTable::resolveImage(ImageBinding handle) const noexcept {
    if (!m_imageSlots) {
        return std::nullopt;
    }
    const auto slot = m_imageSlots->resolve(handle);
    return slot ? std::optional{static_cast<std::uint32_t>(*slot)} : std::nullopt;
}

bool DescriptorHeapTable::retireImage(ImageBinding handle) noexcept {
    if (!m_imageSlots || !m_resources) {
        return false;
    }
    return m_imageSlots->retire(handle, [this](const HeapImageDescriptor& descriptor) noexcept {
        m_resources->releaseSampledImage(descriptor.handle);
    });
}

std::expected<DescriptorHeapTable::SamplerBinding, core::Error>
DescriptorHeapTable::registerSampler(const VkSamplerCreateInfo& sampler) {
    if (!m_samplerSlots) {
        return std::unexpected(invalidState("Descriptor Heap table is not open"));
    }
    auto handle = m_samplerSlots->registerPending(sampler);
    if (!handle) {
        return std::unexpected(invalidState("Descriptor Heap sampler slot capacity exhausted"));
    }
    return *handle;
}

std::optional<std::uint32_t>
DescriptorHeapTable::resolveSampler(SamplerBinding handle) const noexcept {
    if (!m_samplerSlots) {
        return std::nullopt;
    }
    const auto slot = m_samplerSlots->resolve(handle);
    return slot ? std::optional{static_cast<std::uint32_t>(*slot)} : std::nullopt;
}

bool DescriptorHeapTable::retireSampler(SamplerBinding handle) noexcept {
    if (!m_samplerSlots) {
        return false;
    }
    return m_samplerSlots->retire(handle);
}

std::expected<bool, core::Error> DescriptorHeapTable::publishCompleted(bool submissionsComplete) {
    if (!m_imageSlots || !m_samplerSlots || !m_resources) {
        return std::unexpected(invalidState("Descriptor Heap table is not open"));
    }
    if (!submissionsComplete) {
        return false;
    }

    std::optional<core::Error> writeError;
    const bool imagesChanged = m_imageSlots->tryApplyCompleted(
        true,
        true,
        [&](std::size_t slot, const HeapImageDescriptor& descriptor) {
            auto written = writeImageSlot(
                static_cast<std::uint32_t>(slot), descriptor.image, descriptor.format);
            if (!written) {
                writeError = std::move(written.error());
                return false;
            }
            return true;
        },
        [this](const HeapImageDescriptor& descriptor) noexcept {
            m_resources->releaseSampledImage(descriptor.handle);
        });
    if (writeError) {
        return std::unexpected(std::move(*writeError));
    }

    const bool samplersChanged = m_samplerSlots->tryApplyCompleted(
        true,
        true,
        [&](std::size_t slot, const VkSamplerCreateInfo& descriptor) {
            auto written = writeSamplerSlot(static_cast<std::uint32_t>(slot), descriptor);
            if (!written) {
                writeError = std::move(written.error());
                return false;
            }
            return true;
        },
        [](const VkSamplerCreateInfo&) noexcept {});
    if (writeError) {
        return std::unexpected(std::move(*writeError));
    }
    return imagesChanged || samplersChanged;
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
