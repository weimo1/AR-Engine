#pragma once

#include <cstdint>
#include <expected>
#include <cstddef>
#include <span>
#include <vector>

namespace ar::render {

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

constexpr GraphUsageMask usageMask(GraphUsage usage) noexcept {
    return static_cast<GraphUsageMask>(usage);
}

enum class GraphAspect { Color, Depth };

struct GraphImage {
    std::uint32_t mipCount = 1;
    std::uint32_t layerCount = 1;
    GraphAspect aspect = GraphAspect::Color;
};

struct GraphAccess {
    std::uint32_t image;
    std::uint32_t mip;
    std::uint32_t layer;
    GraphUsage usage;
    bool discardContents = false;
};

struct GraphPass {
    std::span<const GraphAccess> accesses;
    bool readsIndirectResources = false;
    std::span<const GraphAccess> indirectReads{};
};

struct GraphSubresourceState {
    std::uint32_t image;
    std::uint32_t mip;
    std::uint32_t layer;
    GraphLayout layout;
    GraphUsageMask usages;
};

struct GraphDeclaration {
    std::span<const GraphImage> images;
    std::span<const GraphPass> passes;
    std::span<const GraphSubresourceState> imports{};
};

struct GraphBarrier {
    GraphAccess target;
    // A discard controls content preservation. Prior accesses remain in this mask.
    GraphUsageMask priorUsages;
    GraphUsageMask nextUsages;
    GraphLayout previousLayout;
    GraphLayout nextLayout;
};

struct GraphPlan {
    // Indexed by declaration order. Empty entries still correspond to a pass.
    std::vector<std::vector<GraphBarrier>> beforePass;
    std::vector<GraphSubresourceState> finalStates;
};

enum class GraphErrorCode {
    UninitializedRead,
    ConflictingAccess,
    OutOfRange,
    InvalidAccess,
    InvalidImport,
    MissingIndirectReads
};

struct GraphError {
    GraphErrorCode code;
    // An invalid import has pass == noPass.
    static constexpr std::size_t noPass = static_cast<std::size_t>(-1);
    std::size_t pass;
    std::uint32_t image;
    std::uint32_t mip;
    std::uint32_t layer;
};

std::expected<GraphPlan, GraphError> planRenderGraph(const GraphDeclaration& declaration);

} // namespace ar::render
