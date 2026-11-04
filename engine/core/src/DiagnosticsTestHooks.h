#pragma once

// Test-only fault injection for the diagnostic sinks. It is compiled into ar_core only when
// AR_ENABLE_TEST_HOOKS is ON, is never part of an installed header, and is not referenced by
// production code paths once the guard is off.
#if defined(AR_ENABLE_TEST_HOOKS)

#include <cstddef>

namespace ar::core::detail {

// Arms the next `count` flush attempts to fail.
void setFlushFaultsForTesting(std::size_t count) noexcept;

// Returns true when the next flush attempt must be treated as failed, decrementing the counter.
bool consumeFlushFaultForTesting() noexcept;

// Number of "flushing diagnostics failed" lines written to stderr so far, so a test can prove
// the warning is reported once per session rather than once per failed flush.
std::size_t flushWarningCountForTesting() noexcept;

// Bumped by the library whenever it writes that warning; internal to the hook pair above.
void countFlushWarningForTesting() noexcept;

} // namespace ar::core::detail

#endif // AR_ENABLE_TEST_HOOKS
