#include "runtime/RuntimeSession.h"

// A public header must compile on its own; this translation unit uses the declared types.
namespace {

ar::core::EndResult makeResult() {
    ar::core::EndResult result;
    result.result = ar::core::ResultKind::Success;
    result.diagnosticsComplete = true;
    result.resourcesReleased = true;
    return result;
}

ar::runtime::StartFailure makeFailure() {
    ar::runtime::StartFailure failure;
    failure.endResult = makeResult();
    failure.diagnosticsAvailable = true;
    return failure;
}

} // namespace
