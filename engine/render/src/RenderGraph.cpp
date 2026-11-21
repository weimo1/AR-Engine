#include "render/RenderGraph.h"

#include <array>
#include <map>
#include <vector>

namespace ar::render {
namespace {

using Subresource = std::array<std::uint32_t, 3>;

struct Use {
    GraphUsage usage;
    GraphLayout layout;
    bool writes;
};

constexpr std::array uses{
    Use{GraphUsage::ColorWrite, GraphLayout::ColorAttachment, true},
    Use{GraphUsage::FragmentSample, GraphLayout::ShaderRead, false},
    Use{GraphUsage::ComputeSample, GraphLayout::ShaderRead, false},
    Use{GraphUsage::DepthWrite, GraphLayout::DepthAttachment, true},
    Use{GraphUsage::CopyRead, GraphLayout::CopySource, false},
    Use{GraphUsage::Present, GraphLayout::Present, false},
};

struct State {
    GraphLayout layout = GraphLayout::Undefined;
    GraphUsageMask readers = 0;
    GraphUsageMask producer = 0;
};

struct PassAccess {
    GraphAccess target;
    const Use* use;
    GraphUsageMask mask;
};

bool inRange(std::span<const GraphImage> images, const Subresource& key) {
    const auto [image, mip, layer] = key;
    return image < images.size() && mip < images[image].mipCount &&
           layer < images[image].layerCount;
}

GraphError error(GraphErrorCode code, std::size_t pass, const Subresource& key) {
    return {code, pass, key[0], key[1], key[2]};
}

bool compatibleAspect(GraphAspect aspect, GraphLayout layout) {
    if (layout == GraphLayout::DepthAttachment) {
        return aspect == GraphAspect::Depth;
    }
    if (layout == GraphLayout::ColorAttachment || layout == GraphLayout::Present) {
        return aspect == GraphAspect::Color;
    }
    return true;
}

bool validImport(const GraphSubresourceState& state) {
    GraphUsageMask allowed = 0;
    for (const auto& use : uses) {
        if (state.layout == use.layout) {
            allowed |= usageMask(use.usage);
        }
    }
    return state.usages != 0 && (state.usages & ~allowed) == 0;
}

} // namespace

std::expected<GraphPlan, GraphError> planRenderGraph(const GraphDeclaration& declaration) {
    std::map<Subresource, State> states;
    for (const auto& resource : declaration.imports) {
        const Subresource key{resource.image, resource.mip, resource.layer};
        if (!inRange(declaration.images, key)) {
            return std::unexpected(error(GraphErrorCode::OutOfRange, GraphError::noPass, key));
        }
        if (!validImport(resource) || states.contains(key) ||
            !compatibleAspect(declaration.images[resource.image].aspect, resource.layout)) {
            return std::unexpected(error(GraphErrorCode::InvalidImport, GraphError::noPass, key));
        }
        const bool writes = resource.layout == GraphLayout::ColorAttachment ||
                            resource.layout == GraphLayout::DepthAttachment;
        states.emplace(key, State{resource.layout, writes ? 0u : resource.usages, resource.usages});
    }

    GraphPlan plan;
    plan.beforePass.resize(declaration.passes.size());
    for (std::size_t passIndex = 0; passIndex < declaration.passes.size(); ++passIndex) {
        std::vector<PassAccess> accesses;
        const auto& pass = declaration.passes[passIndex];
        if (pass.readsIndirectResources && pass.indirectReads.empty()) {
            // The declaration omitted the resource set, so there is no subresource to report.
            return std::unexpected(error(GraphErrorCode::MissingIndirectReads, passIndex, {}));
        }
        accesses.reserve(pass.accesses.size() + pass.indirectReads.size());
        std::map<Subresource, std::size_t> indices;
        const auto registerAccess = [&](const GraphAccess& access,
                                        bool indirect) -> std::expected<void, GraphError> {
            const Subresource key{access.image, access.mip, access.layer};
            if (!inRange(declaration.images, key)) {
                return std::unexpected(error(GraphErrorCode::OutOfRange, passIndex, key));
            }
            const Use* use = nullptr;
            for (const auto& supported : uses) {
                if (supported.usage == access.usage) {
                    use = &supported;
                    break;
                }
            }
            if (!use || (access.discardContents && !use->writes) ||
                (indirect && use->layout != GraphLayout::ShaderRead) ||
                !compatibleAspect(declaration.images[access.image].aspect, use->layout)) {
                return std::unexpected(error(GraphErrorCode::InvalidAccess, passIndex, key));
            }
            auto [index, inserted] = indices.try_emplace(key, accesses.size());
            if (inserted) {
                accesses.push_back({access, use, usageMask(access.usage)});
            } else {
                auto& prior = accesses[index->second];
                if (use->writes || prior.use->writes || use->layout != prior.use->layout) {
                    return std::unexpected(
                        error(GraphErrorCode::ConflictingAccess, passIndex, key));
                }
                prior.mask |= usageMask(access.usage);
            }
            return {};
        };
        for (const auto& access : pass.accesses) {
            if (auto added = registerAccess(access, false); !added) {
                return std::unexpected(added.error());
            }
        }
        for (const auto& access : pass.indirectReads) {
            if (auto added = registerAccess(access, true); !added) {
                return std::unexpected(added.error());
            }
        }

        auto& barriers = plan.beforePass[passIndex];
        for (const auto& access : accesses) {
            const auto& target = access.target;
            const Subresource key{target.image, target.mip, target.layer};
            auto& state = states[key];
            if (!access.use->writes && state.layout == GraphLayout::Undefined) {
                return std::unexpected(error(GraphErrorCode::UninitializedRead, passIndex, key));
            }

            const bool transition = state.layout != access.use->layout;
            const auto active = state.readers ? state.readers : state.producer;
            if (access.use->writes) {
                barriers.push_back({target, active, access.mask, state.layout, access.use->layout});
                state = {access.use->layout, 0, access.mask};
            } else if (transition || (access.mask & ~state.readers) != 0) {
                // An additional reader can require visibility of the producer and ordering
                // after the earlier transition, even when the current layout is compatible.
                barriers.push_back(
                    {target,
                     active | state.producer,
                     access.mask,
                     state.layout,
                     access.use->layout});
                state.readers = transition ? access.mask : state.readers | access.mask;
                state.layout = access.use->layout;
            }
        }
    }

    plan.finalStates.reserve(states.size());
    for (const auto& [key, state] : states) {
        plan.finalStates.push_back(
            {key[0], key[1], key[2], state.layout, state.readers ? state.readers : state.producer});
    }
    return plan;
}

} // namespace ar::render
