#pragma once

#include <expected>

#include "core/Error.h"
#include "render/StaticRenderInput.h"
#include "rhi/StaticSceneDraw.h"

namespace ar::rhi {
class RhiSession;
}

namespace ar::render {

[[nodiscard]] std::expected<rhi::FrameView, core::Error>
buildFrameView(const StaticCamera& camera, bool enableGBuffer);

std::expected<void, core::Error> publishStaticScene(
    rhi::RhiSession& rhi,
    const StaticRenderInput& input,
    bool verifyRollback,
    bool enableGBuffer = false);

} // namespace ar::render
