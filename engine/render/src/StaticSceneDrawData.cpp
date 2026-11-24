#include "render/StaticSceneDrawData.h"

#include <unordered_map>
#include <utility>

#include "StaticMeshGeometry.h"

namespace ar::render {

std::expected<StaticSceneDrawData, core::Error>
packStaticSceneDrawData(const StaticRenderInput& input, StaticDrawLimits limits) {
    if (input.draws.size() > limits.instances) {
        return detail::invalidStaticGeometry("static scene exceeds packed instance capacity");
    }
    StaticSceneDrawData data;
    data.instances.reserve(input.draws.size());
    std::unordered_map<const assets::StaticMesh*, std::uint32_t> meshIndices;
    for (const auto& draw : input.draws) {
        if (!draw.mesh) {
            return detail::invalidStaticGeometry("static draw is missing a mesh");
        }
        auto normalMatrix = detail::computeStaticNormalMatrix(draw.modelMatrix);
        if (!normalMatrix) {
            return std::unexpected(std::move(normalMatrix.error()));
        }
        const auto [entry, inserted] = meshIndices.try_emplace(
            draw.mesh.get(), static_cast<std::uint32_t>(data.meshes.size()));
        if (inserted) {
            const auto& material = draw.mesh->material;
            if (!detail::isFiniteFloat(material.metallicFactor) ||
                !detail::isFiniteFloat(material.roughnessFactor) ||
                !std::ranges::all_of(material.baseColorFactor, detail::isFiniteFloat)) {
                return detail::invalidStaticGeometry("static mesh has non-finite material factors");
            }
            auto range = detail::appendStaticMeshGeometry(
                data.vertices, data.indices, *draw.mesh, limits, true);
            if (!range) {
                return std::unexpected(std::move(range.error()));
            }
            data.meshes.push_back(*range);
            data.materials.push_back(
                {material.baseColorFactor,
                 material.metallicFactor,
                 material.roughnessFactor,
                 draw.mesh});
        }
        const auto index = entry->second;
        data.instances.push_back({draw.objectId, index, index, draw.modelMatrix, *normalMatrix});
    }
    return data;
}

} // namespace ar::render
