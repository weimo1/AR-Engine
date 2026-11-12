#pragma once

#include <cstdint>

namespace ar::rhi::detail {

struct ResourceLifetime {
    std::uint64_t generation = 0;
    bool retired = false;

    [[nodiscard]] bool matches(std::uint64_t value) const noexcept {
        return value != 0 && generation == value && !retired;
    }
    [[nodiscard]] bool reclaimable(bool inUse) const noexcept {
        return retired && !inUse;
    }
    [[nodiscard]] bool free() const noexcept {
        return generation == 0;
    }
};

} // namespace ar::rhi::detail
