#include "DeviceResources.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include "VulkanResult.h"

namespace ar::rhi::detail {
namespace {

// RHI sessions exclusively use the process loader on their owner thread.
// Process-wide generations also reject a handle from a closed session.
std::uint64_t nextGeneration = 1;

core::Error invalid(const char* message) {
    return {
        core::ErrorCategory::InvalidArguments, core::ErrorStage::Capability, message, std::nullopt};
}

core::Error failed(const char* operation, VkResult result) {
    return {
        core::ErrorCategory::InitializationFailed,
        core::ErrorStage::Capability,
        std::string(operation) + " returned " + vkResultName(result),
        core::ErrorOrigin{"vulkan", result}};
}

template <typename Slots> std::expected<std::uint32_t, core::Error> reserveSlot(Slots& slots) {
    if (nextGeneration == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(invalid("resource handle generations exhausted"));
    }
    const auto free =
        std::ranges::find_if(slots, [](const auto& slot) { return slot.lifetime.free(); });
    const auto index = static_cast<std::size_t>(free - slots.begin());
    if (index == slots.size()) {
        if (index == std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(invalid("resource slots exhausted"));
        }
        try {
            slots.emplace_back();
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                core::Error{
                    core::ErrorCategory::InitializationFailed,
                    core::ErrorStage::Capability,
                    "not enough host memory to reserve a resource slot",
                    std::nullopt});
        }
    }
    return static_cast<std::uint32_t>(index);
}

template <typename Slots, typename Handle>
typename Slots::value_type* findSlot(Slots& slots, Handle handle) noexcept {
    if (!handle.generation || handle.slot >= slots.size()) {
        return nullptr;
    }
    auto& slot = slots[handle.slot];
    return slot.lifetime.matches(handle.generation) ? &slot : nullptr;
}

constexpr VkFormat kImageFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags kImageUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

} // namespace

DeviceResources::DeviceResources(
    VkInstance instance,
    VkPhysicalDevice physical,
    VkDevice device,
    VkQueue queue,
    std::uint32_t queueFamily,
    std::uint32_t apiVersion) noexcept
    : m_instance(instance), m_physical(physical), m_device(device), m_queue(queue),
      m_queueFamily(queueFamily), m_apiVersion(apiVersion) {}

DeviceResources::~DeviceResources() {
    if (!m_allocator) {
        return;
    }
    if (m_transferPool) {
        vkDestroyCommandPool(m_device, m_transferPool, nullptr);
    }
    if (m_transferFence) {
        vkDestroyFence(m_device, m_transferFence, nullptr);
    }
    freeStaging();
    for (const auto& slot : m_views) {
        if (slot.view) {
            vkDestroyImageView(m_device, slot.view, nullptr);
        }
    }
    for (const auto& slot : m_images) {
        if (slot.image) {
            vmaDestroyImage(m_allocator, slot.image, slot.allocation);
        }
    }
    for (const auto& slot : m_buffers) {
        if (slot.buffer) {
            vmaDestroyBuffer(m_allocator, slot.buffer, slot.allocation);
        }
    }
    vmaDestroyAllocator(m_allocator);
}

std::expected<void, core::Error> DeviceResources::ensureAllocator() {
    if (m_allocator) {
        return {};
    }
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo info{};
    info.instance = m_instance;
    info.physicalDevice = m_physical;
    info.device = m_device;
    info.vulkanApiVersion = m_apiVersion;
    info.pVulkanFunctions = &functions;
    VmaAllocator allocator = VK_NULL_HANDLE;
    const VkResult result = vmaCreateAllocator(&info, &allocator);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateAllocator", result));
    }
    m_allocator = allocator;
    return {};
}

VkDeviceSize DeviceResources::bufferSizeLimit() {
    if (!m_maxBufferSize) {
        VkPhysicalDeviceMaintenance4Properties limits{};
        limits.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &limits;
        vkGetPhysicalDeviceProperties2(m_physical, &properties);
        m_maxBufferSize = limits.maxBufferSize;
    }
    return m_maxBufferSize;
}

std::expected<BufferHandle, core::Error> DeviceResources::createBuffer(BufferDesc desc) {
    if (!desc.bytes ||
        (desc.memory != BufferMemory::HostVisible && desc.memory != BufferMemory::DeviceLocal)) {
        return std::unexpected(invalid("buffer requires a positive size and a valid memory class"));
    }
    if (desc.bytes > bufferSizeLimit()) {
        return std::unexpected(invalid("buffer size exceeds the device limit"));
    }
    if (auto initialized = ensureAllocator(); !initialized) {
        return std::unexpected(std::move(initialized.error()));
    }
    const auto index = reserveSlot(m_buffers);
    if (!index) {
        return std::unexpected(index.error());
    }
    auto created = allocateBuffer(desc);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }
    created->lifetime.generation = nextGeneration++;
    m_buffers[*index] = *created;
    return BufferHandle{*index, created->lifetime.generation};
}

std::expected<DeviceResources::BufferSlot, core::Error>
DeviceResources::allocateBuffer(BufferDesc desc) {
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = desc.bytes;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    if (desc.memory == BufferMemory::HostVisible) {
        allocation.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    } else {
        allocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
    BufferSlot created;
    const VkResult result = vmaCreateBuffer(
        m_allocator, &buffer, &allocation, &created.buffer, &created.allocation, nullptr);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateBuffer", result));
    }
    created.desc = desc;
    return created;
}

void DeviceResources::freeStaging() noexcept {
    if (m_staging.buffer) {
        vmaDestroyBuffer(m_allocator, m_staging.buffer, m_staging.allocation);
        m_staging = {};
    }
}

void DeviceResources::collectRetired() noexcept {
    for (auto& slot : m_buffers) {
        if (slot.lifetime.reclaimable(m_pendingTransfer && m_transferBuffer == slot.buffer)) {
            vmaDestroyBuffer(m_allocator, slot.buffer, slot.allocation);
            slot = {};
        }
    }
    for (auto& slot : m_images) {
        if (slot.lifetime.reclaimable(m_pendingTransfer && m_transferImage == slot.image)) {
            vmaDestroyImage(m_allocator, slot.image, slot.allocation);
            slot = {};
        }
    }
}

std::expected<DeviceResources::BufferSlot*, core::Error>
DeviceResources::validateAccess(BufferHandle handle, std::uint64_t offset, std::size_t bytes) {
    auto* slot = findSlot(m_buffers, handle);
    if (!slot) {
        return std::unexpected(invalid("buffer handle is invalid or expired"));
    }
    if (slot->desc.memory != BufferMemory::HostVisible) {
        return std::unexpected(invalid("direct CPU access requires a host-visible buffer"));
    }
    if (offset > slot->desc.bytes || bytes > slot->desc.bytes - offset) {
        return std::unexpected(invalid("buffer access is outside the resource range"));
    }
    if (auto settled = settleTransfer(true); !settled) {
        return std::unexpected(std::move(settled.error()));
    }
    return slot;
}

std::expected<void, core::Error> DeviceResources::writeBuffer(
    BufferHandle handle, std::uint64_t offset, std::span<const std::byte> bytes) {
    auto slot = validateAccess(handle, offset, bytes.size());
    if (!slot) {
        return std::unexpected(std::move(slot.error()));
    }
    return writeAllocation((*slot)->allocation, offset, bytes);
}

std::expected<void, core::Error> DeviceResources::writeAllocation(
    VmaAllocation_T* allocation, std::uint64_t offset, std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return {};
    }
    void* mapped = nullptr;
    VkResult result = vmaMapMemory(m_allocator, allocation, &mapped);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaMapMemory", result));
    }
    std::memcpy(static_cast<std::byte*>(mapped) + offset, bytes.data(), bytes.size());
    result = vmaFlushAllocation(m_allocator, allocation, offset, bytes.size());
    vmaUnmapMemory(m_allocator, allocation);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaFlushAllocation", result));
    }
    return {};
}

std::expected<void, core::Error>
DeviceResources::readBuffer(BufferHandle handle, std::uint64_t offset, std::span<std::byte> bytes) {
    auto slot = validateAccess(handle, offset, bytes.size());
    if (!slot) {
        return std::unexpected(std::move(slot.error()));
    }
    return readAllocation((*slot)->allocation, offset, bytes);
}

std::expected<void, core::Error> DeviceResources::readAllocation(
    VmaAllocation_T* allocation, std::uint64_t offset, std::span<std::byte> bytes) {
    if (bytes.empty()) {
        return {};
    }
    void* mapped = nullptr;
    VkResult result = vmaMapMemory(m_allocator, allocation, &mapped);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaMapMemory", result));
    }
    result = vmaInvalidateAllocation(m_allocator, allocation, offset, bytes.size());
    if (result == VK_SUCCESS) {
        std::memcpy(bytes.data(), static_cast<const std::byte*>(mapped) + offset, bytes.size());
    }
    vmaUnmapMemory(m_allocator, allocation);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaInvalidateAllocation", result));
    }
    return {};
}

std::expected<void, core::Error> DeviceResources::destroyBuffer(BufferHandle handle) {
    auto* slot = findSlot(m_buffers, handle);
    if (!slot) {
        return std::unexpected(invalid("buffer handle is invalid or expired"));
    }
    slot->lifetime.retired = true;
    collectRetired();
    return {};
}

std::expected<ImageHandle, core::Error> DeviceResources::createImage(ImageDesc desc) {
    if (!desc.width || !desc.height) {
        return std::unexpected(invalid("image dimensions must be positive"));
    }
    VkImageFormatProperties limits{};
    const VkResult supported = vkGetPhysicalDeviceImageFormatProperties(
        m_physical,
        kImageFormat,
        VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        kImageUsage,
        0,
        &limits);
    if (supported != VK_SUCCESS) {
        return std::unexpected(failed("vkGetPhysicalDeviceImageFormatProperties", supported));
    }
    if (desc.width > limits.maxExtent.width || desc.height > limits.maxExtent.height) {
        return std::unexpected(invalid("image dimensions exceed the device limit"));
    }
    if (auto initialized = ensureAllocator(); !initialized) {
        return std::unexpected(std::move(initialized.error()));
    }
    const auto index = reserveSlot(m_images);
    if (!index) {
        return std::unexpected(index.error());
    }
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = kImageFormat;
    info.extent = {desc.width, desc.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = kImageUsage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    ImageSlot created;
    const VkResult result = vmaCreateImage(
        m_allocator, &info, &allocation, &created.image, &created.allocation, nullptr);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vmaCreateImage", result));
    }
    created.lifetime.generation = nextGeneration++;
    created.desc = desc;
    m_images[*index] = created;
    return ImageHandle{*index, created.lifetime.generation};
}

std::expected<ImageViewHandle, core::Error> DeviceResources::createImageView(ImageHandle image) {
    const auto* source = findSlot(m_images, image);
    if (!source) {
        return std::unexpected(invalid("image handle is invalid or expired"));
    }
    const auto index = reserveSlot(m_views);
    if (!index) {
        return std::unexpected(index.error());
    }
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = source->image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = kImageFormat;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ViewSlot created;
    const VkResult result = vkCreateImageView(m_device, &info, nullptr, &created.view);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed("vkCreateImageView", result));
    }
    created.image = image;
    created.lifetime.generation = nextGeneration++;
    m_views[*index] = created;
    return ImageViewHandle{*index, created.lifetime.generation};
}

std::expected<ImageHandle, core::Error>
DeviceResources::createSampledImage(ImageDesc desc, std::span<const std::byte> pixels) {
    if (static_cast<std::uint64_t>(desc.width) * desc.height != pixels.size() / 4 ||
        pixels.size() % 4 != 0) {
        return std::unexpected(invalid("sampled upload requires the full RGBA8 byte count"));
    }
    auto image = createImage(desc);
    if (!image) {
        return std::unexpected(std::move(image.error()));
    }
    auto uploaded = transfer(
        VK_NULL_HANDLE,
        findSlot(m_images, *image),
        pixels,
        {},
        false,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!uploaded) {
        return std::unexpected(std::move(uploaded.error()));
    }
    if (auto ready = waitTransfer(*uploaded); !ready) {
        return std::unexpected(std::move(ready.error()));
    }
    return *image;
}

std::expected<VkImageView, core::Error> DeviceResources::retainSampledView(ImageViewHandle handle) {
    auto* view = findSlot(m_views, handle);
    if (!view) {
        return std::unexpected(invalid("image view handle is invalid or expired"));
    }
    auto* image = findSlot(m_images, view->image);
    if (!image || image->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        return std::unexpected(invalid("binding requires a sampled image"));
    }
    ++image->bindingReferences;
    ++view->bindingReferences;
    return view->view;
}

std::expected<void, core::Error> DeviceResources::destroyImageView(ImageViewHandle handle) {
    auto* slot = findSlot(m_views, handle);
    if (!slot) {
        return std::unexpected(invalid("image view handle is invalid or expired"));
    }
    if (slot && slot->bindingReferences != 0) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "resource is referenced by the descriptor set",
                std::nullopt});
    }
    vkDestroyImageView(m_device, slot->view, nullptr);
    *slot = {};
    return {};
}

std::expected<void, core::Error> DeviceResources::destroyImage(ImageHandle handle) {
    auto* slot = findSlot(m_images, handle);
    if (!slot) {
        return std::unexpected(invalid("image handle is invalid or expired"));
    }
    if (std::ranges::any_of(
            m_views, [handle](const auto& view) { return view.view && view.image == handle; })) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "destroy the image views before destroying their image",
                std::nullopt});
    }
    slot->lifetime.retired = true;
    collectRetired();
    return {};
}

std::expected<TransferToken, core::Error>
DeviceResources::uploadBuffer(BufferHandle handle, std::span<const std::byte> bytes) {
    const auto* slot = findSlot(m_buffers, handle);
    if (!slot || bytes.size() != slot->desc.bytes) {
        return std::unexpected(invalid("upload requires a live buffer and its full byte count"));
    }
    auto submitted =
        transfer(slot->buffer, nullptr, bytes, {}, slot->desc.memory == BufferMemory::HostVisible);
    if (submitted) {
        m_buffers[handle.slot].uploaded = true;
    }
    return submitted;
}

std::expected<void, core::Error>
DeviceResources::readbackBuffer(BufferHandle handle, std::span<std::byte> bytes) {
    const auto* slot = findSlot(m_buffers, handle);
    if (!slot || !slot->uploaded || bytes.size() != slot->desc.bytes) {
        return std::unexpected(
            invalid("readback requires an uploaded buffer and its full byte count"));
    }
    if (auto result = transfer(slot->buffer, nullptr, {}, bytes); !result) {
        return std::unexpected(std::move(result.error()));
    }
    return {};
}

namespace {
bool matchesImageSize(ImageDesc desc, std::size_t bytes) {
    const auto pixels = static_cast<std::uint64_t>(desc.width) * desc.height;
    return pixels <= std::numeric_limits<std::uint64_t>::max() / 4 && bytes == pixels * 4;
}
} // namespace

std::expected<TransferToken, core::Error>
DeviceResources::uploadImage(ImageHandle handle, std::span<const std::byte> bytes) {
    auto* slot = findSlot(m_images, handle);
    if (slot && slot->bindingReferences != 0) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "resource is referenced by the descriptor set",
                std::nullopt});
    }
    if (!slot || !matchesImageSize(slot->desc, bytes.size())) {
        return std::unexpected(
            invalid("upload requires a live image and its full RGBA8 byte count"));
    }
    return transfer(VK_NULL_HANDLE, slot, bytes, {});
}

std::expected<void, core::Error>
DeviceResources::readbackImage(ImageHandle handle, std::span<std::byte> bytes) {
    auto* slot = findSlot(m_images, handle);
    if (slot && slot->bindingReferences != 0) {
        return std::unexpected(
            core::Error{
                core::ErrorCategory::InvalidState,
                core::ErrorStage::Capability,
                "resource is referenced by the descriptor set",
                std::nullopt});
    }
    if (!slot || slot->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
        !matchesImageSize(slot->desc, bytes.size())) {
        return std::unexpected(
            invalid("readback requires an uploaded image and its full RGBA8 byte count"));
    }
    if (auto result = transfer(VK_NULL_HANDLE, slot, {}, bytes); !result) {
        return std::unexpected(std::move(result.error()));
    }
    return {};
}

} // namespace ar::rhi::detail
