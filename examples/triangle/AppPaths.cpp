#include "AppPaths.h"

#include <cerrno>
#include <cwchar>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ar::app {
namespace {

namespace fs = std::filesystem;

std::string pathToUtf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace

std::expected<AppPaths, std::string> resolveAppPaths(
    const fs::path& executableDirectory,
    const std::optional<fs::path>& explicitOutput,
    const fs::path& defaultOutput) {
    if (executableDirectory.empty()) {
        return std::unexpected("executable directory must not be empty");
    }
    const fs::path output = explicitOutput ? *explicitOutput : defaultOutput;
    if (output.empty()) {
        return std::unexpected("output directory must not be empty");
    }
    return AppPaths{
        executableDirectory / "shaders",
        executableDirectory / "content",
        output,
    };
}

std::expected<fs::path, std::string>
requireReadableFile(const fs::path& path, std::string_view role) {
    if (path.empty()) {
        return std::unexpected(std::string("missing ") + std::string(role) + ": empty path");
    }
    std::error_code error;
    const fs::path expanded = fs::weakly_canonical(path, error);
    const fs::path& display = error ? path : expanded;
    error.clear();
    if (!fs::is_regular_file(path, error) || error) {
        return std::unexpected(
            std::string("missing ") + std::string(role) + ": " + pathToUtf8(display));
    }
    return display;
}

std::expected<fs::path, std::string>
requireReadableDirectory(const fs::path& path, std::string_view role) {
    if (path.empty()) {
        return std::unexpected(std::string("missing ") + std::string(role) + ": empty path");
    }
    std::error_code error;
    const fs::path expanded = fs::weakly_canonical(path, error);
    const fs::path& display = error ? path : expanded;
    error.clear();
    if (!fs::is_directory(path, error) || error) {
        return std::unexpected(
            std::string("missing ") + std::string(role) + ": " + pathToUtf8(display));
    }
    return display;
}

std::expected<fs::path, std::string> currentExecutableDirectory() {
#ifdef _WIN32
    constexpr std::size_t kMaxPathChars = 32768u;
    std::vector<wchar_t> buffer(512u, L'\0');
    while (buffer.size() <= kMaxPathChars) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return std::unexpected("GetModuleFileNameW failed to locate current executable");
        }
        if (written < buffer.size()) {
            fs::path executable(std::wstring_view(buffer.data(), written));
            const fs::path parent = executable.parent_path();
            if (parent.empty()) {
                return std::unexpected("current executable path has no parent directory");
            }
            return parent;
        }
        buffer.resize(buffer.size() * 2u, L'\0');
    }
    return std::unexpected("current executable path exceeds supported buffer limit");
#else
    return std::unexpected("currentExecutableDirectory is only implemented on Windows");
#endif
}

std::optional<LaunchOptions>
parseLaunchOptions(std::span<const std::wstring_view> args, const AppPaths& paths) {
    LaunchOptions options;
    options.output = paths.output;
    bool explicitOutput = false;
    std::optional<fs::path> explicitScene;
    std::optional<fs::path> explicitAssetRoot;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::wstring_view arg = args[index];
        if (arg == L"--descriptor-heap") {
            options.descriptorHeap = true;
        } else if (arg == L"--hidden") {
            options.visible = false;
        } else if (arg == L"--frames" && !options.frames && index + 1 < args.size()) {
            const std::wstring_view value = args[++index];
            if (value.empty() || value.find_first_not_of(L"0123456789") != value.npos) {
                return std::nullopt;
            }
            const std::wstring text(value);
            errno = 0;
            const unsigned long long parsed = std::wcstoull(text.c_str(), nullptr, 10);
            if (errno == ERANGE || parsed == 0 ||
                parsed > std::numeric_limits<std::uint32_t>::max()) {
                return std::nullopt;
            }
            options.frames = static_cast<std::uint32_t>(parsed);
        } else if (arg == L"--scene" && !explicitScene && index + 1 < args.size()) {
            explicitScene = fs::path(args[++index]);
            if (explicitScene->empty()) {
                return std::nullopt;
            }
        } else if (arg == L"--asset-root" && !explicitAssetRoot && index + 1 < args.size()) {
            explicitAssetRoot = fs::path(args[++index]);
            if (explicitAssetRoot->empty()) {
                return std::nullopt;
            }
        } else if (arg == L"--screenshot" && !options.screenshot && index + 1 < args.size()) {
            options.screenshot = fs::path(args[++index]);
            if (options.screenshot->empty()) {
                return std::nullopt;
            }
        } else if (
            arg == L"--gbuffer-probes" && !options.gbufferProbes && index + 1 < args.size()) {
            options.gbufferProbes = fs::path(args[++index]);
            if (options.gbufferProbes->empty()) {
                return std::nullopt;
            }
        } else if (arg == L"--benchmark" && !options.benchmarkSpec && index + 1 < args.size()) {
            options.benchmarkSpec = fs::path(args[++index]);
            if (options.benchmarkSpec->empty()) {
                return std::nullopt;
            }
        } else if (arg == L"--output" && index + 1 < args.size()) {
            options.output = fs::path(args[++index]);
            if (options.output.empty()) {
                return std::nullopt;
            }
            explicitOutput = true;
        } else {
            return std::nullopt;
        }
    }

    if (options.benchmarkSpec) {
        if (options.descriptorHeap || !options.visible || options.frames || explicitScene ||
            explicitAssetRoot || options.screenshot || options.gbufferProbes || explicitOutput) {
            return std::nullopt;
        }
        return options;
    }

    if (explicitScene.has_value() != explicitAssetRoot.has_value()) {
        return std::nullopt;
    }
    if (explicitScene) {
        options.scene = std::move(*explicitScene);
        options.assetRoot = std::move(*explicitAssetRoot);
    } else {
        if (paths.content.empty()) {
            return std::nullopt;
        }
        options.scene = paths.content / "showroom" / "scene.json";
        options.assetRoot = paths.content / "showroom" / "assets";
    }

    if ((!explicitOutput && options.output.empty()) ||
        ((!options.visible || options.screenshot || options.gbufferProbes) && !options.frames)) {
        return std::nullopt;
    }
    return options;
}

std::string_view launchUsageText() noexcept {
    return "usage: ar_engine [--descriptor-heap] [--scene JSON --asset-root DIRECTORY] "
           "[--hidden] [--frames N] [--screenshot PATH] [--gbuffer-probes PATH] "
           "[--output DIRECTORY] | ar_engine --benchmark SPEC_JSON\n";
}

} // namespace ar::app
