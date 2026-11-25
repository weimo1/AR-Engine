#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ar::app {

struct AppPaths {
    std::filesystem::path shaders;
    std::filesystem::path content;
    std::filesystem::path output;

    bool operator==(const AppPaths&) const = default;
};

// Minimal launch options for ar_engine. Interactive desktop startup requires zero arguments and
// opens the built-in showroom with full deferred PBR, IBL, directional shadows, and free camera.
struct LaunchOptions {
    bool descriptorHeap = false;
    bool visible = true;
    std::optional<std::uint32_t> frames;
    std::filesystem::path scene;
    std::filesystem::path assetRoot;
    std::optional<std::filesystem::path> screenshot;
    std::optional<std::filesystem::path> gbufferProbes;
    std::optional<std::filesystem::path> benchmarkSpec;
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
