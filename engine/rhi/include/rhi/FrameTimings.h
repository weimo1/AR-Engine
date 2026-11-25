#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ar::rhi {

// Per-frame CPU timings recorded around one submitted presentation frame.
struct CpuFrameTimings {
    std::uint64_t frameNumber = 0u;
    std::optional<double> cullMs;
    double recordSubmitMs = 0.0;
    double totalCpuMs = 0.0;
    std::optional<std::uint32_t> visibleCount;
    std::optional<std::uint32_t> drawCount;
    bool overflowed = false;
};

// Completed per-frame GPU pass durations harvested from timestamp queries after the frame's
// submission fence signals. Unavailable metrics carry an explicit reason and never spoof zero.
struct GpuFrameTimings {
    std::uint64_t frameNumber = 0u;
    bool available = false;
    std::string unavailableReason;
    std::optional<double> shadowMs;
    std::optional<double> cullMs;
    std::optional<double> geometryMs;
    std::optional<double> lightingMs;
    std::optional<double> displayMs;
    std::optional<double> overlayMs;
    std::optional<double> totalGpuMs;
    std::optional<std::uint32_t> visibleCount;
    std::optional<std::uint32_t> drawCount;
    bool overflowed = false;
};

} // namespace ar::rhi
