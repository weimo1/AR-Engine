#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/Diagnostics.h"
#include "core/Error.h"
#include "rhi/Capabilities.h"
#include "rhi/LoaderSource.h"

namespace ar::rhi {

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
};

struct GraphicsDeviceInfo {
    std::uint32_t graphicsQueueFamily = 0;
    bool validationEnabled = false;
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

    // First call releases the device, messenger, instance, volk and loader. Repeated calls
    // return the cached result without touching the OS again.
    CloseResult close();

private:
    struct Impl;

    explicit RhiSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace ar::rhi
