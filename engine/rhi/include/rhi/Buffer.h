#pragma once

#include <cstdint>

namespace ar::rhi {

enum class BufferMemory { HostVisible, DeviceLocal };

struct BufferDesc {
    std::uint64_t bytes = 0;
    BufferMemory memory = BufferMemory::DeviceLocal;
};

struct BufferHandle {
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    bool operator==(const BufferHandle&) const = default;
};

} // namespace ar::rhi
