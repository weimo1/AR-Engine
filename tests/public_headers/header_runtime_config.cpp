#include "runtime/RuntimeConfig.h"

// A public header must compile on its own; this translation unit uses the declared types.
namespace {

ar::runtime::RuntimeConfig makeConfig() {
    ar::runtime::RuntimeConfig config;
    config.mode = ar::runtime::QueryMode::RequireProduct;
    config.diagnostics.runId = "header-probe";
    config.query.loader = ar::rhi::SystemLoader{};
    config.query.requestedApi = ar::rhi::kApiVersion1_4;
    config.query.device = ar::rhi::FirstEnumerated{};
    return config;
}

} // namespace
