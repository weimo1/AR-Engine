#pragma once

#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "core/Diagnostics.h"
#include "core/Error.h"
#include "rhi/Capabilities.h"
#include "runtime/RuntimeConfig.h"

namespace ar::runtime {

// Owned result of a failed start. The caller learns what was established, what was rolled back and
// which diagnostic run ended, without guessing which module exists.
struct StartFailure {
    // Verdict of the failed start. result carries the business outcome, primary_error the earliest
    // cause, cleanup_errors the cleanup failures and diagnostic_errors the output failures.
    core::EndResult endResult;
    // Whatever the query had established. Unexecuted stages stay at their not-queried values.
    rhi::CapabilityReport report;
    // False when the diagnostic session could not be created, so no run directory exists.
    bool diagnosticsAvailable = false;
};

// Owns diagnostics, an optional platform window and one RHI session.
// Single owner thread, not copyable and not movable, so the address stays stable for rhi.
class RuntimeSession {
public:
    static std::expected<std::unique_ptr<RuntimeSession>, StartFailure>
    start(const RuntimeConfig& config);

    ~RuntimeSession();

    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;
    RuntimeSession(RuntimeSession&&) = delete;
    RuntimeSession& operator=(RuntimeSession&&) = delete;

    // Runtime-owned snapshot: readable after close and unchanged by it. The reference dies with
    // the session, so copy before destroying it.
    [[nodiscard]] const rhi::CapabilityReport& report() const noexcept;

    // True after accepting one presentation, false when the window was closed.
    [[nodiscard]] std::expected<bool, core::Error> presentClearFrame();

    // First call closes rhi, then finishes the diagnostic run. Repeated calls return the cached
    // result without cleaning again or writing a second summary.
    core::EndResult close();

private:
    struct Impl;

    explicit RuntimeSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace ar::runtime
