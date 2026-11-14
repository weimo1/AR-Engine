#pragma once

#include <array>
#include <cstddef>
#include <optional>

namespace ar::rhi::detail {

// Slot zero is reserved at table creation. Other slots use its descriptor until published.
template <typename Descriptor, std::size_t Capacity> class SetBindingSlots {
    static_assert(Capacity > 0);

public:
    explicit SetBindingSlots(Descriptor fallback) {
        m_slots[0] = fallback;
    }

    [[nodiscard]] std::optional<std::size_t> reserve(Descriptor descriptor) {
        for (std::size_t index = 1; index < Capacity; ++index) {
            if (!m_slots[index]) {
                m_slots[index] = descriptor;
                return index;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const Descriptor& descriptor(std::size_t index) const noexcept {
        return m_slots[index] ? *m_slots[index] : *m_slots[0];
    }

private:
    std::array<std::optional<Descriptor>, Capacity> m_slots;
};

} // namespace ar::rhi::detail
