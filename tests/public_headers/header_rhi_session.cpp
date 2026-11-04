#include "rhi/RhiSession.h"

// A public header must compile on its own; this translation unit uses the declared types.
namespace {

ar::rhi::QueryConfig makeConfig() {
    ar::rhi::QueryConfig config;
    config.loader = ar::rhi::SystemLoader{};
    config.requestedApi = ar::rhi::kApiVersion1_4;
    config.device = ar::rhi::FirstEnumerated{};
    return config;
}

ar::rhi::CloseResult makeClosed() {
    ar::rhi::CloseResult result;
    result.closed = true;
    result.resourcesReleased = true;
    return result;
}

} // namespace
