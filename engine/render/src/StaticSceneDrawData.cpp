#include "render/StaticSceneDrawData.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/matrix.hpp>

#include "assets/StaticAssetCache.h"

namespace ar::render {
namespace {

std::unexpected<core::Error> invalid(const char* message) {
    return std::unexpected(
        core::Error{
            core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation, message, {}});
}

bool finite(float value) {
    return std::isfinite(value);
}

std::expected<StaticMeshRange, core::Error>
appendMesh(StaticSceneDrawData& data, const assets::StaticMesh& mesh, StaticDrawLimits limits) {
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0 ||
        (!mesh.normals.empty() && mesh.normals.size() != mesh.vertices.size()) ||
        std::ranges::any_of(
            mesh.indices, [&](auto index) { return index >= mesh.vertices.size(); })) {
        return invalid("invalid static mesh topology");
    }
    auto minimum = mesh.vertices.front().position;
    auto maximum = minimum;
    for (const auto& vertex : mesh.vertices) {
        if (!std::ranges::all_of(vertex.position, finite) ||
            !std::ranges::all_of(vertex.uv, finite)) {
            return invalid("non-finite static vertex");
        }
        for (std::size_t axis = 0; axis < minimum.size(); ++axis) {
            minimum[axis] = std::min(minimum[axis], vertex.position[axis]);
            maximum[axis] = std::max(maximum[axis], vertex.position[axis]);
        }
    }
    const auto firstVertex = data.vertices.size();
    const auto firstIndex = data.indices.size();
    const auto vertexLimit = std::min(
        limits.vertices, static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));
    if (!mesh.normals.empty()) {
        if (mesh.vertices.size() > vertexLimit - firstVertex ||
            mesh.indices.size() > limits.indices - firstIndex) {
            return invalid("static scene geometry exceeds packed capacity");
        }
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            if (!std::ranges::all_of(mesh.normals[i], finite)) {
                return invalid("non-finite static vertex normal");
            }
            data.vertices.push_back(
                {mesh.vertices[i].position, mesh.vertices[i].uv, mesh.normals[i]});
        }
        data.indices.insert(data.indices.end(), mesh.indices.begin(), mesh.indices.end());
    } else {
        for (std::size_t triangle = 0; triangle < mesh.indices.size(); triangle += 3) {
            const auto& a = mesh.vertices[mesh.indices[triangle]];
            const auto& b = mesh.vertices[mesh.indices[triangle + 1]];
            const auto& c = mesh.vertices[mesh.indices[triangle + 2]];
            const auto origin = glm::make_vec3(a.position.data());
            const auto cross = glm::cross(
                glm::make_vec3(b.position.data()) - origin,
                glm::make_vec3(c.position.data()) - origin);
            const float length = glm::length(cross);
            if (!(length > 0.F) || !finite(length)) {
                continue;
            }
            if (vertexLimit - data.vertices.size() < 3 ||
                limits.indices - data.indices.size() < 3) {
                return invalid("static scene geometry exceeds packed capacity");
            }
            const auto unit = cross / length;
            const std::array normal{unit.x, unit.y, unit.z};
            for (const auto* vertex : {&a, &b, &c}) {
                data.indices.push_back(
                    static_cast<std::uint32_t>(data.vertices.size() - firstVertex));
                data.vertices.push_back({vertex->position, vertex->uv, normal});
            }
        }
    }
    if (data.indices.size() == firstIndex) {
        return invalid("static mesh has no drawable triangles");
    }
    return StaticMeshRange{
        static_cast<std::uint32_t>(firstIndex),
        static_cast<std::uint32_t>(data.indices.size() - firstIndex),
        static_cast<std::int32_t>(firstVertex),
        minimum,
        maximum};
}

} // namespace

std::expected<StaticSceneDrawData, core::Error>
packStaticSceneDrawData(const StaticRenderInput& input, StaticDrawLimits limits) {
    if (input.draws.size() > limits.instances) {
        return invalid("static scene exceeds packed instance capacity");
    }
    StaticSceneDrawData data;
    data.instances.reserve(input.draws.size());
    std::unordered_map<const assets::StaticMesh*, std::uint32_t> meshIndices;
    for (const auto& draw : input.draws) {
        if (!draw.mesh || !std::ranges::all_of(draw.modelMatrix, finite)) {
            return invalid("static draw is missing a mesh or has a non-finite transform");
        }
        const glm::mat3 linear(glm::make_mat4(draw.modelMatrix.data()));
        const auto determinant = glm::determinant(linear);
        if (!finite(determinant) || determinant == 0.F) {
            return invalid("static draw transform must be invertible");
        }
        const auto normal = glm::transpose(glm::inverse(linear));
        std::array<float, 12> normalMatrix{};
        for (std::size_t column = 0; column < 3; ++column) {
            std::copy_n(glm::value_ptr(normal) + column * 3, 3, normalMatrix.begin() + column * 4);
        }
        if (!std::ranges::all_of(normalMatrix, finite)) {
            return invalid("static draw has non-finite inverse transpose");
        }
        const auto [entry, inserted] = meshIndices.try_emplace(
            draw.mesh.get(), static_cast<std::uint32_t>(data.meshes.size()));
        if (inserted) {
            auto range = appendMesh(data, *draw.mesh, limits);
            if (!range) {
                return std::unexpected(std::move(range.error()));
            }
            data.meshes.push_back(*range);
            const auto& material = draw.mesh->material;
            data.materials.push_back(
                {material.baseColorFactor,
                 material.metallicFactor,
                 material.roughnessFactor,
                 draw.mesh});
        }
        const auto index = entry->second;
        data.instances.push_back({draw.objectId, index, index, draw.modelMatrix, normalMatrix});
    }
    return data;
}

} // namespace ar::render
