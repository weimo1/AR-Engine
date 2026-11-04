// Compiled alone to prove core/Error.h is self-contained.
#include "core/Error.h"

namespace ar::test {

const char* errorMessageOf(const ar::core::Error& error)
{
    return error.message.c_str();
}

} // namespace ar::test
