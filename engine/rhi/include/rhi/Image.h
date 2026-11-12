#pragma once

#include <cstdint>

namespace ar::rhi {

// A single-mip, single-layer RGBA8 UNORM image.
struct ImageDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct ImageHandle {
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    bool operator==(const ImageHandle&) const = default;
};

struct ImageViewHandle {
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    bool operator==(const ImageViewHandle&) const = default;
};

} // namespace ar::rhi
