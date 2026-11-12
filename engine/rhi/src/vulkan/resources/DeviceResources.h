#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

#include <volk.h>

#include "core/Error.h"
#include "rhi/Buffer.h"
#include "rhi/Image.h"
#include "rhi/RhiSession.h"
#include "ResourceLifetime.h"

struct VmaAllocation_T;
struct VmaAllocator_T;

namespace ar::rhi::detail {

// Borrows a live Vulkan device and its parents. Destroy on the session owner thread
// before destroying the device.
class DeviceResources {
public:
    DeviceResources(
        VkInstance instance,
        VkPhysicalDevice physical,
        VkDevice device,
        VkQueue queue,
        std::uint32_t queueFamily,
        std::uint32_t apiVersion) noexcept;
    ~DeviceResources();

    DeviceResources(const DeviceResources&) = delete;
    DeviceResources& operator=(const DeviceResources&) = delete;

    std::expected<BufferHandle, core::Error> createBuffer(BufferDesc desc);
    std::expected<void, core::Error>
    writeBuffer(BufferHandle handle, std::uint64_t offset, std::span<const std::byte> bytes);
    std::expected<void, core::Error>
    readBuffer(BufferHandle handle, std::uint64_t offset, std::span<std::byte> bytes);
    std::expected<void, core::Error> destroyBuffer(BufferHandle handle);
    std::expected<ImageHandle, core::Error> createImage(ImageDesc desc);
    std::expected<ImageViewHandle, core::Error> createImageView(ImageHandle image);
    std::expected<void, core::Error> destroyImageView(ImageViewHandle handle);
    std::expected<void, core::Error> destroyImage(ImageHandle handle);
    std::expected<TransferToken, core::Error>
        uploadBuffer(BufferHandle, std::span<const std::byte>);
    std::expected<TransferToken, core::Error> uploadImage(ImageHandle, std::span<const std::byte>);
    std::expected<void, core::Error> readbackBuffer(BufferHandle, std::span<std::byte>);
    std::expected<void, core::Error> readbackImage(ImageHandle, std::span<std::byte>);
    std::expected<bool, core::Error> pollTransfer(TransferToken);
    std::expected<void, core::Error> waitTransfer(TransferToken);
    std::expected<bool, core::Error> settleTransfer(bool wait);
    [[nodiscard]] bool hasPendingTransfer() const noexcept {
        return m_pendingTransfer;
    }
    void abandon() noexcept {
        m_allocator = nullptr;
    }

private:
    struct BufferSlot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        BufferDesc desc{};
        ResourceLifetime lifetime;
        bool uploaded = false;
    };

    struct ImageSlot {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        ResourceLifetime lifetime;
        ImageDesc desc{};
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct ViewSlot {
        VkImageView view = VK_NULL_HANDLE;
        ImageHandle image{};
        ResourceLifetime lifetime;
    };

    std::expected<void, core::Error> ensureAllocator();
    VkDeviceSize bufferSizeLimit();
    std::expected<BufferSlot, core::Error> allocateBuffer(BufferDesc desc);
    void freeStaging() noexcept;
    void collectRetired() noexcept;
    std::expected<void, core::Error>
    writeAllocation(VmaAllocation_T*, std::uint64_t, std::span<const std::byte>);
    std::expected<void, core::Error>
    readAllocation(VmaAllocation_T*, std::uint64_t, std::span<std::byte>);
    std::expected<void, core::Error> ensureTransferCommands();
    std::expected<TransferToken, core::Error> transfer(
        VkBuffer buffer,
        ImageSlot* image,
        std::span<const std::byte> input,
        std::span<std::byte> output,
        bool hostVisibleDestination = false);
    std::expected<BufferSlot*, core::Error>
    validateAccess(BufferHandle handle, std::uint64_t offset, std::size_t bytes);

    VkInstance m_instance;
    VkPhysicalDevice m_physical;
    VkDevice m_device;
    VkQueue m_queue;
    std::uint32_t m_queueFamily;
    std::uint32_t m_apiVersion;
    VkDeviceSize m_maxBufferSize = 0;
    VmaAllocator_T* m_allocator = nullptr;
    std::vector<BufferSlot> m_buffers;
    std::vector<ImageSlot> m_images;
    std::vector<ViewSlot> m_views;
    VkCommandPool m_transferPool = VK_NULL_HANDLE;
    VkCommandBuffer m_transferCommands = VK_NULL_HANDLE;
    VkFence m_transferFence = VK_NULL_HANDLE;
    BufferSlot m_staging{};
    std::uint64_t m_firstTransfer = 0;
    std::uint64_t m_lastTransfer = 0;
    std::uint64_t m_completedTransfer = 0;
    bool m_pendingTransfer = false;
    VkBuffer m_transferBuffer = VK_NULL_HANDLE;
    VkImage m_transferImage = VK_NULL_HANDLE;
    bool m_readbackNeedsStaging = false;
    bool m_transferLost = false;
};

} // namespace ar::rhi::detail
