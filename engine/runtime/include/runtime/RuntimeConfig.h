#pragma once

#include "core/Diagnostics.h"
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

// Everything a start needs. No window, scene or frame parameters exist in P0.
struct RuntimeConfig {
    QueryMode mode = QueryMode::Inspect;
    core::DiagnosticConfig diagnostics;
    rhi::QueryConfig query;
};

} // namespace ar::runtime
