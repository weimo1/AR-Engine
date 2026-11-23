#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <volk.h>

#include "VulkanResult.h"
#include "core/Error.h"
#include "rhi/PresentationSpec.h"

namespace ar::rhi::detail::presentation {

inline constexpr std::uint64_t kFrameTimeout = 5'000'000'000;

constexpr VkFormat toVkFormat(AttachmentFormat format) noexcept {
    switch (format) {
    case AttachmentFormat::Rgba8Unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case AttachmentFormat::Rgba16Sfloat:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case AttachmentFormat::Rg8Unorm:
        return VK_FORMAT_R8G8_UNORM;
    case AttachmentFormat::D32Sfloat:
        return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

constexpr std::uint32_t formatTexelBytes(AttachmentFormat format) noexcept {
    switch (format) {
    case AttachmentFormat::Rgba8Unorm:
        return 4u;
    case AttachmentFormat::Rgba16Sfloat:
        return 8u;
    case AttachmentFormat::Rg8Unorm:
        return 2u;
    case AttachmentFormat::D32Sfloat:
        return 4u;
    }
    return 0u;
}

constexpr VkDeviceSize probeStart(VkDeviceSize screenshotBytes) noexcept {
    return (screenshotBytes + 7) & ~VkDeviceSize{7};
}

inline core::Error failed(const char* operation, VkResult result) {
    return {
        core::ErrorCategory::InitializationFailed,
        core::ErrorStage::Capability,
        std::string(operation) + " returned " + vkResultName(result),
        core::ErrorOrigin{"vulkan", result},
    };
}

inline core::Error unavailable(const char* message) {
    return {core::ErrorCategory::QueryFailed, core::ErrorStage::Capability, message, std::nullopt};
}

template <class Handle, class Info, class Create>
std::expected<void, core::Error>
create(VkDevice device, Handle& handle, const Info& info, Create function, const char* name) {
    Handle created = VK_NULL_HANDLE;
    const VkResult result = function(device, &info, nullptr, &created);
    if (result != VK_SUCCESS) {
        return std::unexpected(failed(name, result));
    }
    handle = created;
    return {};
}

} // namespace ar::rhi::detail::presentation
