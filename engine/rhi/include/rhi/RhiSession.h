#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "core/Diagnostics.h"
#include "core/Error.h"
#include "rhi/Capabilities.h"
#include "rhi/Image.h"
#include "rhi/Buffer.h"
#include "rhi/LoaderSource.h"

namespace ar::rhi {

struct TransferToken {
    std::uint64_t value = 0;
};

inline constexpr std::uint32_t
makeApiVersion(std::uint32_t major, std::uint32_t minor, std::uint32_t patch = 0) noexcept {
    return (major << 22) | (minor << 12) | patch;
}

inline constexpr std::uint32_t kApiVersion1_4 = makeApiVersion(1, 4);

// Picks the first enumerated device for queries, or the first compatible graphics device.
struct FirstEnumerated {};

// Picks one enumerated index; an out-of-range index is invalid_arguments and never falls back.
struct ExplicitDeviceIndex {
    std::uint32_t index = 0;
};

using DeviceSelection = std::variant<FirstEnumerated, ExplicitDeviceIndex>;

struct TriangleShaders {
    std::vector<std::uint32_t> vertex;
    std::vector<std::uint32_t> fragment;
};

struct FrameScreenshot {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

enum class PresentStatus { Presented, Retry };

struct Win32Presentation {
    // Borrowed HWND. Keep the window alive through close, or until process exit if
    // close reports a retained Presentation.
    void* windowHandle = nullptr;
    std::vector<std::string> instanceExtensions;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Everything open needs. Query mode (inspect / require-product / development) is runtime policy
// and deliberately absent here.
struct QueryConfig {
    LoaderSource loader = SystemLoader{};
    std::uint32_t requestedApi = kApiVersion1_4;
    DeviceSelection device = FirstEnumerated{};
    // Optional device extensions the caller requires. A missing entry only clears the
    // required_capabilities_supported judgment; product eligibility is runtime's decision.
    std::vector<std::string> requiredDeviceExtensions;
    bool createGraphicsDevice = false;
    // Opt-in for a Vulkan 1.4 graphics device. Unsupported requests fail explicitly.
    bool enableDescriptorHeap = false;
    std::optional<Win32Presentation> presentation;
    std::optional<TriangleShaders> triangleShaders;
    bool enableScreenshot = false;
    bool enableSetBinding = false;
};

struct DescriptorHeapLimits {
    std::uint64_t samplerHeapAlignment = 0;
    std::uint64_t resourceHeapAlignment = 0;
    std::uint64_t maxSamplerHeapSize = 0;
    std::uint64_t maxResourceHeapSize = 0;
    std::uint64_t minSamplerHeapReservedRange = 0;
    std::uint64_t minResourceHeapReservedRange = 0;
};

struct GraphicsDeviceInfo {
    std::uint32_t graphicsQueueFamily = 0;
    bool validationEnabled = false;
    // Populated only after enabling the extension and resolving the required device commands.
    std::optional<DescriptorHeapLimits> descriptorHeap;
};

// Returned when open cannot produce a session. The report holds whatever was already established
// and is owned by the caller. A rollback that could not release everything lists the leftovers in
// residualResources, so the caller's summary can state them instead of implying a clean release.
struct QueryFailure {
    core::Error error;
    std::vector<core::Error> cleanupErrors;
    std::vector<core::ResidualResource> residualResources;
    CapabilityReport report;
};

struct CloseResult {
    bool closed = false;
    bool resourcesReleased = true;
    std::vector<core::ResidualResource> residualResources;
    std::vector<core::Error> errors;
};

// Owns the Vulkan loader, instance and optional graphics device. One live session per process;
// use it on its owner thread and keep diagnostics alive through close().
class RhiSession {
public:
    static std::expected<std::unique_ptr<RhiSession>, QueryFailure>
    open(const QueryConfig& config, core::DiagnosticSession& diagnostics);

    ~RhiSession();

    RhiSession(const RhiSession&) = delete;
    RhiSession& operator=(const RhiSession&) = delete;
    RhiSession(RhiSession&&) = delete;
    RhiSession& operator=(RhiSession&&) = delete;

    // Valid until close(); copy first if the report must outlive the session.
    [[nodiscard]] const CapabilityReport& report() const noexcept;
    [[nodiscard]] std::optional<GraphicsDeviceInfo> graphicsDeviceInfo() const noexcept;

    // Presented counts one accepted presentation, including VK_SUBOPTIMAL_KHR.
    // Retry requests another call with the latest framebuffer size and does not update screenshot.
    // A failed frame stops further submissions until the session is closed.
    [[nodiscard]] std::expected<PresentStatus, core::Error>
    presentFrame(std::uint32_t width, std::uint32_t height, FrameScreenshot* screenshot = nullptr);

    [[nodiscard]] std::expected<BufferHandle, core::Error> createBuffer(BufferDesc desc);
    // Byte offsets refer to the logical size. Only HostVisible buffers allow direct CPU access.
    [[nodiscard]] std::expected<void, core::Error>
    writeBuffer(BufferHandle handle, std::uint64_t offset, std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<void, core::Error>
    readBuffer(BufferHandle handle, std::uint64_t offset, std::span<std::byte> bytes);
    [[nodiscard]] std::expected<void, core::Error> destroyBuffer(BufferHandle handle);

    [[nodiscard]] std::expected<ImageHandle, core::Error> createImage(ImageDesc desc);
    [[nodiscard]] std::expected<ImageViewHandle, core::Error> createImageView(ImageHandle image);
    [[nodiscard]] std::expected<void, core::Error> destroyImageView(ImageViewHandle handle);
    [[nodiscard]] std::expected<void, core::Error> destroyImage(ImageHandle handle);

    [[nodiscard]] std::expected<TransferToken, core::Error>
    uploadBuffer(BufferHandle handle, std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<TransferToken, core::Error>
    uploadImage(ImageHandle handle, std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<void, core::Error>
    readbackBuffer(BufferHandle handle, std::span<std::byte> bytes);
    [[nodiscard]] std::expected<void, core::Error>
    readbackImage(ImageHandle handle, std::span<std::byte> bytes);
    [[nodiscard]] std::expected<bool, core::Error> pollTransfer(TransferToken token);
    [[nodiscard]] std::expected<void, core::Error> waitTransfer(TransferToken token);

    // First call closes presentation, device, surface, messenger, instance, volk and loader.
    // Repeated calls return the cached result without touching the OS again.
    CloseResult close();

private:
    struct Impl;

    explicit RhiSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace ar::rhi
