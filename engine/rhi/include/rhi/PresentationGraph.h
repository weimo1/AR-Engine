#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

#include "core/Error.h"

namespace ar::rhi {

enum class GraphUsage : std::uint32_t {
    ColorWrite = 1u << 0,
    FragmentSample = 1u << 1,
    ComputeSample = 1u << 2,
    DepthWrite = 1u << 3,
    CopyRead = 1u << 4,
    Present = 1u << 5,
};
enum class GraphLayout {
    Undefined,
    ColorAttachment,
    DepthAttachment,
    ShaderRead,
    CopySource,
    Present
};
using GraphUsageMask = std::uint32_t;

enum class PresentationImage : std::uint32_t {
    Output,
    Albedo,
    Normal,
    Material,
    Depth,
    Hdr,
    Count
};
enum class PresentationPass { Geometry, Lighting, Display, Copy, Present, Count };

struct PresentationImageState {
    GraphLayout layout{};
    GraphUsageMask usages = 0;
};
using PresentationStates =
    std::array<PresentationImageState, static_cast<std::size_t>(PresentationImage::Count)>;

struct PresentationGraphInput {
    PresentationStates states;
    bool lighting;
    bool capture;
    bool environment;
};

struct PresentationBarrier {
    PresentationImage image;
    GraphLayout before;
    GraphLayout after;
    GraphUsageMask source;
    GraphUsageMask destination;
    bool discard;
};

struct PresentationGraphPlan {
    std::array<std::vector<PresentationBarrier>, static_cast<std::size_t>(PresentationPass::Count)>
        barriers;
    PresentationStates finalStates;
};

using PlanPresentationGraph =
    std::expected<PresentationGraphPlan, core::Error> (*)(const PresentationGraphInput&);

} // namespace ar::rhi
