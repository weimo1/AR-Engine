#include "AppPaths.h"

#include <cerrno>
#include <cmath>
#include <cwchar>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
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

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::wstring_view arg = args[index];
        if (arg == L"--hidden") {
            options.visible = false;
        } else if (arg == L"--showroom") {
            options.showroom = true;
        } else if (arg == L"--set-binding-retirement") {
            options.setBinding = true;
            options.verifyBindingRetirement = true;
        } else if (arg == L"--gpu-driven" || arg == L"--gpu-driven-empty") {
            options.gpuDriven = true;
            options.gpuEmpty = arg == L"--gpu-driven-empty";
        } else if (arg == L"--set-binding") {
            options.setBinding = true;
        } else if (arg == L"--descriptor-heap") {
            options.descriptorHeap = true;
            options.gbuffer = true;
        } else if (arg == L"--clear-only") {
            options.clearOnly = true;
        } else if (arg == L"--ibl" && !options.ibl && index + 1 < args.size()) {
            const std::wstring_view preset = args[++index];
            if (preset == L"constant") {
                options.ibl = ar::render::EnvironmentPreset::Constant;
            } else if (preset == L"directional") {
                options.ibl = ar::render::EnvironmentPreset::Directional;
            } else {
                return std::nullopt;
            }
            options.hdrLighting = true;
            options.gbuffer = true;
        } else if (arg == L"--verify-ibl-rollback") {
            options.verifyIblRollback = true;
        } else if (arg == L"--hdr-lighting") {
            options.hdrLighting = true;
            options.gbuffer = true;
        } else if (arg == L"--exposure-ev" && !options.exposureEV && index + 1 < args.size()) {
            const std::wstring text(args[++index]);
            wchar_t* end = nullptr;
            errno = 0;
            const float exposure = std::wcstof(text.c_str(), &end);
            if (end == text.c_str() || *end != L'\0' || errno == ERANGE ||
                !std::isfinite(exposure) || std::abs(exposure) > 16.F) {
                return std::nullopt;
            }
            options.exposureEV = exposure;
        } else if (arg == L"--static-gpu-driven") {
            options.staticGpuDriven = true;
            options.bindlessMaterials = true;
            options.gbuffer = true;
        } else if (arg == L"--bindless-materials") {
            options.bindlessMaterials = true;
            options.gbuffer = true;
        } else if (arg == L"--gbuffer") {
            options.gbuffer = true;
        } else if (
            arg == L"--gbuffer-probes" && !options.gbufferProbes && index + 1 < args.size()) {
            options.gbuffer = true;
            options.gbufferProbes = fs::path(args[++index]);
        } else if (arg == L"--static-scene" && !options.scene && index + 1 < args.size()) {
            options.scene = fs::path(args[++index]);
        } else if (arg == L"--asset-root" && !options.assetRoot && index + 1 < args.size()) {
            options.assetRoot = fs::path(args[++index]);
        } else if (
            arg == L"--static-scene-test" && !options.sceneCandidate && index + 1 < args.size()) {
            options.sceneCandidate = fs::path(args[++index]);
        } else if (
            (arg == L"--frames" || arg == L"--indirect-capacity") && index + 1 < args.size()) {
            auto& target = arg == L"--frames" ? options.frames : options.indirectCapacity;
            if (target) {
                return std::nullopt;
            }
            const std::wstring_view value = args[++index];
            if (value.empty() || value.find_first_not_of(L"0123456789") != value.npos) {
                return std::nullopt;
            }
            const std::wstring text(value);
            errno = 0;
            const unsigned long long parsed = std::wcstoull(text.c_str(), nullptr, 10);
            if (errno == ERANGE || parsed > std::numeric_limits<std::uint32_t>::max() ||
                (arg == L"--frames" && parsed == 0)) {
                return std::nullopt;
            }
            target = static_cast<std::uint32_t>(parsed);
        } else if (arg == L"--screenshot" && !options.screenshot && index + 1 < args.size()) {
            options.screenshot = fs::path(args[++index]);
            if (options.screenshot->empty()) {
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

    if (options.showroom) {
        if (options.scene || options.assetRoot || options.sceneCandidate || options.clearOnly ||
            options.setBinding || options.verifyBindingRetirement || options.gpuDriven ||
            paths.content.empty()) {
            return std::nullopt;
        }
        options.scene = paths.content / "showroom" / "scene.json";
        options.assetRoot = paths.content / "showroom" / "assets";
        options.gbuffer = true;
        options.bindlessMaterials = true;
        options.staticGpuDriven = true;
        options.hdrLighting = true;
        if (!options.ibl) {
            options.ibl = ar::render::EnvironmentPreset::Directional;
        }
    }

    if ((!explicitOutput && options.output.empty()) ||
        ((!options.visible || options.screenshot || options.gbufferProbes) && !options.frames)) {
        return std::nullopt;
    }
    if (options.verifyBindingRetirement && (options.frames != 3 || options.screenshot)) {
        return std::nullopt;
    }
    if (options.clearOnly && options.setBinding) {
        return std::nullopt;
    }
    if (options.gpuDriven && (options.clearOnly || options.setBinding)) {
        return std::nullopt;
    }
    if (options.descriptorHeap && (options.setBinding || options.clearOnly || options.gpuDriven)) {
        return std::nullopt;
    }
    if ((options.indirectCapacity && !options.staticGpuDriven) ||
        (options.verifyIblRollback && (!options.ibl || !options.frames || *options.frames < 2)) ||
        (options.exposureEV && !options.hdrLighting) || (options.gbuffer && !options.scene) ||
        (options.gbufferProbes && options.gbufferProbes->empty()) ||
        options.scene.has_value() != options.assetRoot.has_value() ||
        (options.scene && (options.scene->empty() || options.assetRoot->empty() ||
                           options.gpuDriven || options.clearOnly || options.setBinding)) ||
        (options.sceneCandidate &&
         (!options.scene || !options.screenshot || options.frames != 4))) {
        return std::nullopt;
    }
    return options;
}

std::string_view launchUsageText() noexcept {
    return "usage: ar_triangle [--showroom] [--frames N] [--hidden] [--clear-only] "
           "[--set-binding] [--descriptor-heap] [--static-scene JSON --asset-root DIRECTORY] "
           "[--static-gpu-driven [--indirect-capacity N]] "
           "[--gbuffer [--gbuffer-probes PATH]] [--bindless-materials] [--hdr-lighting "
           "[--exposure-ev EV]] "
           "[--ibl constant|directional] [--screenshot PATH] [--output DIRECTORY]\n";
}

} // namespace ar::app
