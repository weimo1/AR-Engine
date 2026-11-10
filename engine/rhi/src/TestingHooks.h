#pragma once

// Test-only access to facts that the public rhi interface deliberately does not expose:
// the process quarantine flag, the number of load/volk/instance attempts, and report() calls
// made after close. Everything here is compiled only when AR_ENABLE_TEST_HOOKS is defined.

#ifdef AR_ENABLE_TEST_HOOKS

#include <cstddef>
#include <vector>

namespace ar::rhi::detail {

struct ProcessCounters {
    std::size_t loadCalls = 0;
    std::size_t volkInitializeCalls = 0;
    std::size_t instanceCreateCalls = 0;
    std::size_t destroyInstanceCalls = 0;
    std::size_t volkFinalizeCalls = 0;
    std::size_t unloadCalls = 0;
    std::size_t reportCallsAfterClose = 0;
    // Counts the quarantine probes that run before any OS load, so a caller can prove the check
    // was skipped (0) rather than inferring it from the flag being clear.
    std::size_t quarantineChecks = 0;
};

bool isProcessQuarantined() noexcept;

void clearProcessQuarantine() noexcept;

ProcessCounters processCounters() noexcept;

void resetProcessCounters() noexcept;

// Makes the next unload of a loader module fail, so a cleanup failure can be observed without
// corrupting a real DLL. The module stays loaded until process exit.
void failNextUnloadForTesting() noexcept;

// Release steps recorded by shutdown(), in order: 1 = instance destroy attempt, 2 = volk
// cleanup, 3 = module unload attempt. A caller asserts the sequence, not just the totals.
std::vector<int> processShutdownOrder();

} // namespace ar::rhi::detail

#endif // AR_ENABLE_TEST_HOOKS
