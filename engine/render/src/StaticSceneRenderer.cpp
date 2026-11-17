#include "render/StaticSceneRenderer.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "assets/StaticAssetCache.h"
#include "rhi/RhiSession.h"

namespace ar::render {

std::expected<void, core::Error> publishStaticScene(
    rhi::RhiSession& rhi, const StaticRenderInput& input, bool verifyRollback, bool enableGBuffer) {
    const auto& camera = input.camera;
    const auto& [px, py, pz] = camera.position;
    const auto& [x, y, z, w] = camera.orientation;
    const auto orientation = glm::normalize(glm::quat(w, x, y, z));
    const auto view = glm::mat4_cast(glm::conjugate(orientation)) *
                      glm::translate(glm::mat4(1.F), {-px, -py, -pz});
    auto projection = glm::perspectiveRH_ZO(
        camera.verticalFovRadians, 1.F, camera.farPlane, camera.nearPlane);
    if (!enableGBuffer) {
        projection[1][1] *= -1.F;
    }
    const auto clipFromWorld = projection * view;
    std::vector<std::vector<rhi::StaticVertex>> vertices;
    std::vector<std::vector<std::uint32_t>> faceIndices;
    std::vector<rhi::StaticMeshUpload> meshes;
    std::vector<rhi::StaticInstanceDraw> draws;
    std::unordered_map<const assets::StaticMesh*, std::uint32_t> indices;
    draws.reserve(input.draws.size());
    for (const auto& draw : input.draws) {
        const auto& asset = draw.mesh;
        if (!asset) {
            return std::unexpected(core::Error{
                core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation,
                "static draw has no mesh asset", {}});
        }
        auto [entry, inserted] = indices.try_emplace(asset.get(), static_cast<std::uint32_t>(meshes.size()));
        if (inserted) {
            auto& buffer = vertices.emplace_back();
            auto& fallback = faceIndices.emplace_back();
            std::span<const std::uint32_t> meshIndices = asset->indices;
            if (enableGBuffer && asset->normals.empty()) {
                buffer.reserve(asset->indices.size());
                fallback.reserve(asset->indices.size());
                for (std::size_t triangle = 0; triangle < asset->indices.size(); triangle += 3) {
                    const auto& a = asset->vertices[asset->indices[triangle]];
                    const auto& b = asset->vertices[asset->indices[triangle + 1]];
                    const auto& c = asset->vertices[asset->indices[triangle + 2]];
                    const glm::vec3 edge1 = glm::make_vec3(b.position.data()) - glm::make_vec3(a.position.data());
                    const glm::vec3 edge2 = glm::make_vec3(c.position.data()) - glm::make_vec3(a.position.data());
                    const auto cross = glm::cross(edge1, edge2);
                    const auto length = glm::length(cross);
                    if (!(length > 0.F) || !std::isfinite(length)) {
                        continue;
                    }
                    const auto normal = cross / length;
                    for (const auto* vertex : {&a, &b, &c}) {
                        fallback.push_back(static_cast<std::uint32_t>(buffer.size()));
                        buffer.push_back({vertex->position, vertex->uv, {normal.x, normal.y, normal.z}});
                    }
                }
                meshIndices = fallback;
            } else {
                buffer.reserve(asset->vertices.size());
                for (std::size_t vertexIndex = 0; vertexIndex < asset->vertices.size(); ++vertexIndex) {
                    const auto& vertex = asset->vertices[vertexIndex];
                    const auto normal = enableGBuffer ? asset->normals[vertexIndex] : std::array<float, 3>{};
                    buffer.push_back({vertex.position, vertex.uv, normal});
                }
            }
            auto& mesh = meshes.emplace_back(rhi::StaticMeshUpload{
                asset, buffer, meshIndices, asset->rgba, asset->width, asset->height});
            if (enableGBuffer) {
                static constexpr std::array<std::uint8_t, 4> white{255, 255, 255, 255};
                const auto& data = asset->material.metallicRoughness;
                mesh.materialRgba = data ? std::span<const std::uint8_t>{data->rgba} : white;
                mesh.materialWidth = data ? data->width : 1;
                mesh.materialHeight = data ? data->height : 1;
            }
        }
        const auto model = glm::make_mat4(draw.modelMatrix.data());
        const auto clip = clipFromWorld * model;
        auto& instance = draws.emplace_back(rhi::StaticInstanceDraw{entry->second, {}});
        std::copy_n(glm::value_ptr(clip), instance.clipFromModel.size(), instance.clipFromModel.begin());
        if (enableGBuffer) {
            const glm::mat3 linear(view * model);
            const auto determinant = glm::determinant(linear);
            if (!std::isfinite(determinant) || determinant == 0.F) {
                return std::unexpected(core::Error{
                    core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation,
                    "GBuffer requires an invertible instance transform", {}});
            }
            const auto normal = glm::transpose(glm::inverse(linear));
            for (std::size_t column = 0; column < 3; ++column) {
                std::copy_n(glm::value_ptr(normal) + column * 3, 3,
                            instance.normalFromModel.begin() + column * 4);
            }
            instance.normalFromModel[3] = asset->material.metallicFactor;
            instance.normalFromModel[7] = asset->material.roughnessFactor;
            instance.baseColorFactor = asset->material.baseColorFactor;
        }
    }
    const auto inverse = glm::inverse(projection);
    std::array<float, 16> inverseProjection;
    std::copy_n(glm::value_ptr(inverse), inverseProjection.size(), inverseProjection.begin());
    return rhi.replaceStaticScene(meshes, draws, verifyRollback, inverseProjection);
}

} // namespace ar::render
