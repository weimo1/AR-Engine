#include "render/StaticSceneRenderer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "assets/StaticAssetCache.h"
#include "render/StaticSceneDrawData.h"
#include "rhi/RhiSession.h"

namespace ar::render {
namespace {

bool allFinite(std::span<const float> values) noexcept {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}

core::Error invalidCamera(const char* message) {
    return core::Error{
        core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation, message, {}};
}

} // namespace

std::expected<rhi::FrameView, core::Error>
buildFrameView(const StaticCamera& camera, bool enableGBuffer) {
    if (!allFinite(camera.position) || !allFinite(camera.orientation)) {
        return std::unexpected(invalidCamera("camera position or orientation is non-finite"));
    }
    if (!std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) ||
        !(camera.nearPlane > 0.F) || !(camera.farPlane > camera.nearPlane)) {
        return std::unexpected(invalidCamera("camera clip planes require 0 < near < far"));
    }
    if (!std::isfinite(camera.verticalFovRadians) || !(camera.verticalFovRadians > 0.F) ||
        !(camera.verticalFovRadians < std::numbers::pi_v<float>)) {
        return std::unexpected(invalidCamera("camera vertical FOV must be in (0, pi)"));
    }
    const auto& [px, py, pz] = camera.position;
    const auto& [x, y, z, w] = camera.orientation;
    const float lengthSquared = x * x + y * y + z * z + w * w;
    if (!std::isfinite(lengthSquared) || !(lengthSquared > 0.F)) {
        return std::unexpected(invalidCamera("camera orientation quaternion must be non-zero"));
    }
    const float invLength = 1.F / std::sqrt(lengthSquared);
    if (!std::isfinite(invLength)) {
        return std::unexpected(invalidCamera("camera orientation quaternion cannot be normalized"));
    }
    const glm::quat orientation(w * invLength, x * invLength, y * invLength, z * invLength);
    const auto view = glm::mat4_cast(glm::conjugate(orientation)) *
                      glm::translate(glm::mat4(1.F), {-px, -py, -pz});
    auto projection = glm::perspectiveRH_ZO(
        camera.verticalFovRadians, 1.F, camera.farPlane, camera.nearPlane);
    if (!enableGBuffer) {
        projection[1][1] *= -1.F;
    }
    const auto clipFromWorld = projection * view;
    const auto inverseProjection = glm::inverse(projection);
    const auto cameraRotation = glm::mat4_cast(orientation);

    rhi::FrameView result{};
    std::copy_n(glm::value_ptr(view), result.view.size(), result.view.begin());
    std::copy_n(
        glm::value_ptr(clipFromWorld), result.clipFromWorld.size(), result.clipFromWorld.begin());
    std::copy_n(
        glm::value_ptr(inverseProjection),
        result.inverseProjection.size(),
        result.inverseProjection.begin());
    std::copy_n(
        glm::value_ptr(cameraRotation), result.viewToWorld.size(), result.viewToWorld.begin());
    if (!allFinite(result.view) || !allFinite(result.clipFromWorld) ||
        !allFinite(result.inverseProjection) || !allFinite(result.viewToWorld)) {
        return std::unexpected(invalidCamera("camera parameters produce non-finite view matrices"));
    }
    return result;
}

std::expected<void, core::Error> publishStaticScene(
    rhi::RhiSession& rhi, const StaticRenderInput& input, bool verifyRollback, bool enableGBuffer) {
    auto frameView = buildFrameView(input.camera, enableGBuffer);
    if (!frameView) {
        return std::unexpected(std::move(frameView.error()));
    }
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
        auto& instance =
            draws.emplace_back(rhi::StaticInstanceDraw{entry->second, draw.modelMatrix});
        if (enableGBuffer) {
            const glm::mat3 linear(glm::make_mat4(draw.modelMatrix.data()));
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
    std::expected<void, core::Error> replaced;
    if (rhi.staticIndirectEnabled()) {
        const auto packed = packStaticSceneDrawData(input);
        if (!packed) {
            return std::unexpected(packed.error());
        }
        std::vector<rhi::StaticVertex> sharedVertices;
        sharedVertices.reserve(packed->vertices.size());
        for (const auto& vertex : packed->vertices) {
            sharedVertices.push_back({vertex.position, vertex.uv, vertex.normal});
        }
        std::vector<rhi::StaticIndexedRange> ranges;
        ranges.reserve(packed->meshes.size());
        for (const auto& mesh : packed->meshes) {
            ranges.push_back(
                {mesh.firstIndex, mesh.indexCount, mesh.vertexOffset, mesh.minimum, mesh.maximum});
        }
        std::vector<std::uint64_t> objectIds;
        objectIds.reserve(packed->instances.size());
        for (const auto& instance : packed->instances) {
            objectIds.push_back(instance.objectId);
        }
        const rhi::StaticIndirectGeometry geometry{
            sharedVertices, packed->indices, ranges, objectIds};
        replaced = rhi.replaceStaticScene(meshes, draws, verifyRollback, &geometry);
    } else {
        replaced = rhi.replaceStaticScene(meshes, draws, verifyRollback);
    }
    if (!replaced) {
        return replaced;
    }
    return rhi.setFrameView(*frameView);
}

} // namespace ar::render
