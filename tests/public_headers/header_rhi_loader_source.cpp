#include "rhi/LoaderSource.h"

// A public header must compile on its own; this translation unit uses the declared types.
namespace {

ar::rhi::LoaderSource makeSystemSource() {
    return ar::rhi::SystemLoader{};
}

ar::rhi::LoaderSource makeExplicitSource() {
    return ar::rhi::ExplicitLoaderPath{"fixture.dll"};
}

} // namespace
