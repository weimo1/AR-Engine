#pragma once

#include <expected>

#include "core/Error.h"
#include "render/StaticRenderInput.h"

namespace ar::rhi {
class RhiSession;
}

namespace ar::render {

std::expected<void, core::Error> publishStaticScene(
    rhi::RhiSession& rhi, const StaticRenderInput& input, bool verifyRollback);

} // namespace ar::render
