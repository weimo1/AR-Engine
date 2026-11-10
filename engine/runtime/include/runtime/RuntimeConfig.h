#pragma once

#include <optional>

#include "core/Diagnostics.h"
#include "platform/Window.h"
#include "rhi/RhiSession.h"

namespace ar::runtime {

// P0 query policy. rhi only reports facts; the mode decides which facts are required.
enum class QueryMode {
    // Accepts API 1.3 or 1.4. A complete query succeeds even when the product target is not met.
    Inspect,
    // Requires a 1.4 query and a met product target; otherwise the start fails as unsupported.
    RequireProduct,
    // Requires an explicit API 1.3 query. Never downgrades an unspecified request.
    Development,
};

// Leaving window unset keeps the surface-free query and device paths.
struct RuntimeConfig {
    QueryMode mode = QueryMode::Inspect;
    core::DiagnosticConfig diagnostics;
    rhi::QueryConfig query;
    std::optional<platform::WindowConfig> window;
};

} // namespace ar::runtime
