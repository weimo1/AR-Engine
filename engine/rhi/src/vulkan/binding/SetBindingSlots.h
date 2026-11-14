#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace ar::rhi::detail {

template <typename Descriptor, std::size_t Capacity> class SetBindingSlots {
    static_assert(Capacity > 0);

    enum class State { Free, Pending, Published, Retiring };
    struct Slot {
        std::optional<Descriptor> value;
        std::uint64_t generation = 0;
        State state = State::Free;
    };

public:
    struct Handle {
        std::size_t slot;
        std::uint64_t generation;
    };

    explicit SetBindingSlots(Descriptor fallback) {
        m_slots[0].value = fallback;
        m_slots[0].state = State::Published;
    }

    [[nodiscard]] std::optional<Handle> registerPending(Descriptor descriptor) {
        if (m_generation == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::nullopt;
        }
        for (std::size_t index = 1; index < Capacity; ++index) {
            if (m_slots[index].state == State::Free) {
                m_slots[index] = {descriptor, ++m_generation, State::Pending};
                return Handle{index, m_generation};
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::size_t> resolve(Handle handle) const noexcept {
        const auto* slot = find(handle);
        return slot && slot->state == State::Published ? std::optional{handle.slot} : std::nullopt;
    }

    template <typename Release> bool retire(Handle handle, Release release) {
        const auto* slot = find(handle);
        if (!slot) {
            return false;
        }
        if (slot->state == State::Pending) {
            release(*slot->value);
            m_slots[handle.slot] = {};
            return true;
        }
        if (slot->state == State::Published) {
            m_slots[handle.slot].state = State::Retiring;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool hasChanges() const noexcept {
        for (const auto& slot : m_slots) {
            if (slot.state == State::Pending || slot.state == State::Retiring) {
                return true;
            }
        }
        return false;
    }

    bool retire(Handle handle) {
        return retire(handle, [](const Descriptor&) {});
    }

    template <typename Write, typename Release>
    bool applyCompleted(
        bool firstFrameComplete, bool secondFrameComplete, Write write, Release release) {
        if (!firstFrameComplete || !secondFrameComplete) {
            return false;
        }
        bool changed = false;
        std::array<std::optional<Descriptor>, Capacity> retired{};
        std::size_t retiredCount = 0;
        for (auto& slot : m_slots) {
            if (slot.state == State::Pending) {
                slot.state = State::Published;
                changed = true;
            } else if (slot.state == State::Retiring) {
                retired[retiredCount++] = *slot.value;
                slot = {};
                changed = true;
            }
        }
        if (changed) {
            write();
            for (std::size_t index = 0; index < retiredCount; ++index) {
                release(*retired[index]);
            }
        }
        return changed;
    }

    bool applyCompleted(bool firstFrameComplete, bool secondFrameComplete) {
        return applyCompleted(
            firstFrameComplete, secondFrameComplete, [] {}, [](const Descriptor&) {});
    }

    [[nodiscard]] const Descriptor& descriptor(std::size_t index) const noexcept {
        if (index < Capacity) {
            const auto& slot = m_slots[index];
            if (slot.state == State::Published || slot.state == State::Retiring) {
                return *slot.value;
            }
        }
        return *m_slots[0].value;
    }

private:
    [[nodiscard]] const Slot* find(Handle handle) const noexcept {
        if (handle.slot == 0 || handle.slot >= Capacity || handle.generation == 0) {
            return nullptr;
        }
        const auto& slot = m_slots[handle.slot];
        return slot.generation == handle.generation ? &slot : nullptr;
    }

    std::array<Slot, Capacity> m_slots{};
    std::uint64_t m_generation = 0;
};

} // namespace ar::rhi::detail
