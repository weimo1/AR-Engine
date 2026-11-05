#pragma once

// Test-only access to facts that the public rhi interface deliberately does not expose:
// the process quarantine flag, the number of load/volk/instance attempts, and report() calls
// made after close. Everything here is compiled only when AR_ENABLE_TEST_HOOKS is defined.

#ifdef AR_ENABLE_TEST_HOOKS

#include <cstddef>

namespace ar::rhi::detail {

struct ProcessCounters {
    std::size_t loadCalls = 0;
    std::size_t volkInitializeCalls = 0;
    std::size_t instanceCreateCalls = 0;
    std::size_t reportCallsAfterClose = 0;
};

bool isProcessQuarantined() noexcept;

void clearProcessQuarantine() noexcept;

ProcessCounters processCounters() noexcept;

void resetProcessCounters() noexcept;

} // namespace ar::rhi::detail

#endif // AR_ENABLE_TEST_HOOKS
