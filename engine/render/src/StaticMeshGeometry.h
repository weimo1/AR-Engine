#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/matrix.hpp>

#include "assets/StaticAssetCache.h"
#include "core/Error.h"
#include "render/StaticSceneDrawData.h"

namespace ar::render::detail {

inline std::unexpected<core::Error> invalidStaticGeometry(const char* message) {
    return std::unexpected(
        core::Error{
            core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation, message, {}});
}

inline bool isFiniteFloat(float value) noexcept {
    return std::isfinite(value);
}

inline std::expected<std::array<float, 12>, core::Error>
computeStaticNormalMatrix(const std::array<float, 16>& modelMatrix) {
    if (!std::ranges::all_of(modelMatrix, isFiniteFloat)) {
        return invalidStaticGeometry("static draw has a non-finite transform");
    }
    const glm::mat3 linear(glm::make_mat4(modelMatrix.data()));
    const auto determinant = glm::determinant(linear);
    if (!isFiniteFloat(determinant) || determinant == 0.F) {
        return invalidStaticGeometry("static draw transform must be invertible");
    }
    const auto normal = glm::transpose(glm::inverse(linear));
    std::array<float, 12> normalMatrix{};
    for (std::size_t column = 0; column < 3; ++column) {
        std::copy_n(glm::value_ptr(normal) + column * 3, 3, normalMatrix.begin() + column * 4);
    }
    if (!std::ranges::all_of(normalMatrix, isFiniteFloat)) {
        return invalidStaticGeometry("static draw has non-finite inverse transpose");
    }
    return normalMatrix;
}

inline std::expected<std::pair<std::array<float, 3>, std::array<float, 3>>, core::Error>
validateStaticMeshTopology(const assets::StaticMesh& mesh, bool checkFiniteNormals) {
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0 ||
        (!mesh.normals.empty() && mesh.normals.size() != mesh.vertices.size()) ||
        std::ranges::any_of(
            mesh.indices, [&](std::uint32_t index) { return index >= mesh.vertices.size(); })) {
        return invalidStaticGeometry("invalid static mesh topology");
    }
    auto minimum = mesh.vertices.front().position;
    auto maximum = minimum;
    for (const auto& vertex : mesh.vertices) {
        if (!std::ranges::all_of(vertex.position, isFiniteFloat) ||
            !std::ranges::all_of(vertex.uv, isFiniteFloat)) {
            return invalidStaticGeometry("non-finite static vertex");
        }
        for (std::size_t axis = 0; axis < minimum.size(); ++axis) {
            minimum[axis] = (std::min)(minimum[axis], vertex.position[axis]);
            maximum[axis] = (std::max)(maximum[axis], vertex.position[axis]);
        }
    }
    if (checkFiniteNormals && !mesh.normals.empty()) {
        for (const auto& normal : mesh.normals) {
            if (!std::ranges::all_of(normal, isFiniteFloat)) {
                return invalidStaticGeometry("non-finite static vertex normal");
            }
        }
    }
    return std::pair{minimum, maximum};
}

template <typename VertexT>
std::expected<StaticMeshRange, core::Error> appendStaticMeshGeometry(
    std::vector<VertexT>& outVertices,
    std::vector<std::uint32_t>& outIndices,
    const assets::StaticMesh& mesh,
    StaticDrawLimits limits,
    bool needNormals) {
    const auto bounds = validateStaticMeshTopology(mesh, needNormals);
    if (!bounds) {
        return std::unexpected(bounds.error());
    }
    const auto [minimum, maximum] = *bounds;
    const auto firstVertex = outVertices.size();
    const auto firstIndex = outIndices.size();
    const auto vertexLimit =
        (std::min)(limits.vertices,
                   static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()));

    if (!needNormals || !mesh.normals.empty()) {
        if (firstVertex > vertexLimit || mesh.vertices.size() > vertexLimit - firstVertex ||
            firstIndex > limits.indices || mesh.indices.size() > limits.indices - firstIndex) {
            return invalidStaticGeometry("static scene geometry exceeds packed capacity");
        }
        // Only pre-reserve for fresh single-mesh buffers; shared multi-mesh buffers rely on
        // vector's geometric growth to avoid quadratic reallocation across many small meshes.
        if (firstVertex == 0 && firstIndex == 0) {
            outVertices.reserve(mesh.vertices.size());
            outIndices.reserve(mesh.indices.size());
        }
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const auto& vertex = mesh.vertices[i];
            const auto normal = needNormals ? mesh.normals[i] : std::array<float, 3>{};
            outVertices.push_back(VertexT{vertex.position, vertex.uv, normal});
        }
        outIndices.insert(outIndices.end(), mesh.indices.begin(), mesh.indices.end());
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
            if (!(length > 0.F) || !isFiniteFloat(length)) {
                continue;
            }
            if (outVertices.size() > vertexLimit || vertexLimit - outVertices.size() < 3 ||
                outIndices.size() > limits.indices || limits.indices - outIndices.size() < 3) {
                return invalidStaticGeometry("static scene geometry exceeds packed capacity");
            }
            const auto unit = cross / length;
            const std::array normal{unit.x, unit.y, unit.z};
            for (const auto* vertex : {&a, &b, &c}) {
                outIndices.push_back(static_cast<std::uint32_t>(outVertices.size() - firstVertex));
                outVertices.push_back(VertexT{vertex->position, vertex->uv, normal});
            }
        }
    }
    if (outIndices.size() == firstIndex) {
        return invalidStaticGeometry("static mesh has no drawable triangles");
    }
    return StaticMeshRange{
        static_cast<std::uint32_t>(firstIndex),
        static_cast<std::uint32_t>(outIndices.size() - firstIndex),
        static_cast<std::int32_t>(firstVertex),
        minimum,
        maximum};
}

} // namespace ar::render::detail
