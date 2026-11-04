#pragma once

#include <optional>
#include <string>

#include "core/Error.h"

namespace ar::core {

// Source stamp of the compiled binary. dirty keeps "unknown" distinct from a clean tree.
struct SourceStamp {
    std::optional<std::string> revision;
    std::optional<bool> dirty;

    bool operator==(const SourceStamp& other) const;
};

// Identity compiled into the binary by CMake from build-identity.json. Runtime arguments must
// not override these values.
struct BuildInfo {
    std::string buildId;
    std::string inputHash;
    std::string configuration;
    SourceStamp source;
};

// Defined in the generated translation unit.
const BuildInfo& buildInfo();

} // namespace ar::core
