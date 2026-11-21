#pragma once

#include "rhi/PresentationGraph.h"

namespace ar::render {

std::expected<rhi::PresentationGraphPlan, core::Error>
planPresentationGraph(const rhi::PresentationGraphInput& input);

} // namespace ar::render
