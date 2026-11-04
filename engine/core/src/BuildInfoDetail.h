#pragma once

// Private to ar_core. Exposes the identity values that are only used by build verification
// tooling, never by the public API.

namespace ar::core::detail {

// "/MDd" for debug builds, "/MD" otherwise, derived from the compiler's own preprocessor state.
const char* buildCrtName();

// The vcpkg triplet recorded in the identity sidecar this binary was compiled from.
const char* buildTriplet();

} // namespace ar::core::detail
