#pragma once

#include <optional>
#include <string>

#include "core/BuildInfo.h"
#include "core/Error.h"
#include "rhi/Capabilities.h"
#include "runtime/RuntimeConfig.h"

namespace ar::runtime::detail {

// Result of turning the runtime snapshot into capability_json. Exactly one member is set: either
// the UTF-8 object text handed to core, or the encoding error that core records as an output
// failure. Encoding never silently falls back to an empty object.
struct EncodedCapability {
    std::optional<std::string> json;
    std::optional<core::Error> error;
};

// Encodes the D1 report shape. core only re-validates the syntax and the top-level object type.
EncodedCapability encodeCapability(
    const rhi::CapabilityReport& report,
    QueryMode mode,
    const std::string& runId,
    const core::BuildInfo& build);

} // namespace ar::runtime::detail
