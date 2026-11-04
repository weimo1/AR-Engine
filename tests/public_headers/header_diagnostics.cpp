// Compiled alone to prove core/Diagnostics.h is self-contained.
#include "core/Diagnostics.h"

namespace ar::test {

bool infoEnabled(const ar::core::DiagnosticSession& session)
{
    return session.enabled(ar::core::LogLevel::Info);
}

} // namespace ar::test
