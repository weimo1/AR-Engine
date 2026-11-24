#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "render/EnvironmentLighting.h"

namespace ar::app {

struct AppPaths {
    std::filesystem::path shaders;
    std::filesystem::path content;
    std::filesystem::path output;

    bool operator==(const AppPaths&) const = default;
};

struct LaunchOptions {
    std::optional<std::uint32_t> frames;
    bool visible = true;
    bool showroom = false;
    bool clearOnly = false;
    bool gpuDriven = false;
    bool gpuEmpty = false;
    bool setBinding = false;
    bool descriptorHeap = false;
    bool verifyBindingRetirement = false;
    bool gbuffer = false;
    bool bindlessMaterials = false;
    bool staticGpuDriven = false;
    std::optional<std::uint32_t> indirectCapacity;
    bool hdrLighting = false;
    bool shadows = false;
    std::optional<std::uint32_t> shadowResolution;
    bool freeCamera = false;
    bool verifyIblRollback = false;
    std::optional<ar::render::EnvironmentPreset> ibl;
    std::optional<float> exposureEV;
    std::optional<std::filesystem::path> gbufferProbes;
    std::optional<std::filesystem::path> scene;
    std::optional<std::filesystem::path> assetRoot;
    std::optional<std::filesystem::path> sceneCandidate;
    std::optional<std::filesystem::path> screenshot;
    std::filesystem::path output;
};

[[nodiscard]] std::expected<AppPaths, std::string> resolveAppPaths(
    const std::filesystem::path& executableDirectory,
    const std::optional<std::filesystem::path>& explicitOutput,
    const std::filesystem::path& defaultOutput);

[[nodiscard]] std::expected<std::filesystem::path, std::string>
requireReadableFile(const std::filesystem::path& path, std::string_view role);

[[nodiscard]] std::expected<std::filesystem::path, std::string>
requireReadableDirectory(const std::filesystem::path& path, std::string_view role);

[[nodiscard]] std::expected<std::filesystem::path, std::string> currentExecutableDirectory();

[[nodiscard]] std::optional<LaunchOptions>
parseLaunchOptions(std::span<const std::wstring_view> args, const AppPaths& paths);

[[nodiscard]] std::string_view launchUsageText() noexcept;

} // namespace ar::app
