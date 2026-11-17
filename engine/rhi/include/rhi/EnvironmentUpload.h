#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace ar::rhi {

// Packed binary16 texels. Cube data is mip-major, then face-major, then row-major.
struct EnvironmentUpload {
    std::span<const std::uint16_t> diffuse;
    std::span<const std::uint16_t> specular;
    std::span<const std::uint16_t> brdf;
};

} // namespace ar::rhi
