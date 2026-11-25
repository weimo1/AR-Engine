#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/Error.h"
#include "render/StaticRenderInput.h"
#include "rhi/PresentationSpec.h"

namespace ar::runtime {

inline constexpr std::uint32_t kObservationSettingsVersion = 1u;

struct ObservationSettings {
    std::uint32_t version = kObservationSettingsVersion;
    std::string name;
    // Relative scene identifier (forward-slash normalized, e.g. "showroom/scene.json").
    std::string sceneRef;
    render::StaticCamera camera{};
    float exposureEV = 0.F;
    render::DirectionalLight directionalLight{};
    rhi::DebugViewMode viewMode = rhi::DebugViewMode::Final;

    bool operator==(const ObservationSettings&) const = default;
};

[[nodiscard]] std::string_view debugViewModeName(rhi::DebugViewMode mode) noexcept;
[[nodiscard]] std::optional<rhi::DebugViewMode> parseDebugViewMode(std::string_view name) noexcept;

[[nodiscard]] std::expected<void, core::Error>
validateObservationSettings(const ObservationSettings& settings);

// Validates and writes to a temporary sibling file before atomically replacing destination.
// On failure, any existing destination file is left untouched.
[[nodiscard]] std::expected<void, core::Error> saveObservationSettings(
    const std::filesystem::path& destination, const ObservationSettings& settings);

// Loads, parses, and validates an observation preset. When expectedSceneRef is provided, a
// mismatched sceneRef is rejected with InvalidArguments and a clear diagnostic reason.
[[nodiscard]] std::expected<ObservationSettings, core::Error> loadObservationSettings(
    const std::filesystem::path& source,
    std::optional<std::string_view> expectedSceneRef = std::nullopt);

} // namespace ar::runtime
