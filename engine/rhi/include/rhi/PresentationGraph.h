#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

#include "core/Error.h"
#include "rhi/PresentationSpec.h"

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
    ShadowDepth,
    Count
};
enum class PresentationPass { Shadow, Geometry, Lighting, Display, Overlay, Copy, Present, Count };
enum class IndirectProducer { None, Cpu, GpuCompute };
enum class IndirectBuffer { Commands, Counts };
enum class IndirectUse { Clear, ComputeWrite, ComputeReadWrite, DrawRead, HostRead };
enum class IndirectPhase { Cull, Geometry, Readback, Count };

struct IndirectBufferBarrier {
    IndirectBuffer resource;
    IndirectUse source;
    IndirectUse destination;
};

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
    IndirectProducer indirectProducer = IndirectProducer::None;
    bool shadows = false;
    DebugViewMode debugView = DebugViewMode::Final;
    bool overlay = false;
    bool readIndirectCounts = false;
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
    std::array<std::vector<IndirectBufferBarrier>, static_cast<std::size_t>(IndirectPhase::Count)>
        indirectBarriers;
};

using PlanPresentationGraph =
    std::expected<PresentationGraphPlan, core::Error> (*)(const PresentationGraphInput&);

} // namespace ar::rhi
