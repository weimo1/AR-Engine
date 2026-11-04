#pragma once

#include <filesystem>
#include <variant>

namespace ar::rhi {

// The real Vulkan loader resolved by the OS search path (vulkan-1.dll).
struct SystemLoader {};

// A loader the caller points at explicitly, for example a task-owned fixture DLL.
// The path is owned by the config; open copies it and never re-reads the environment.
struct ExplicitLoaderPath {
    std::filesystem::path path;
};

using LoaderSource = std::variant<SystemLoader, ExplicitLoaderPath>;

} // namespace ar::rhi
