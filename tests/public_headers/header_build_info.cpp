// Compiled alone to prove core/BuildInfo.h is self-contained.
#include "core/BuildInfo.h"

namespace ar::test {

const char* buildIdOf(const ar::core::BuildInfo& info) {
    return info.buildId.c_str();
}

} // namespace ar::test
