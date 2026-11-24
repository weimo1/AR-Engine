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

#include "StaticMeshGeometry.h"
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
    auto projection =
        glm::perspectiveRH_ZO(camera.verticalFovRadians, 1.F, camera.farPlane, camera.nearPlane);
    if (!enableGBuffer) {
        projection[1][1] *= -1.F;
    }
    const auto clipFromWorld = projection * view;
    const auto inverseProjection = glm::inverse(projection);
    const auto viewToWorld =
        glm::translate(glm::mat4(1.F), {px, py, pz}) * glm::mat4_cast(orientation);

    rhi::FrameView result{};
    std::copy_n(glm::value_ptr(view), result.view.size(), result.view.begin());
    std::copy_n(
        glm::value_ptr(clipFromWorld), result.clipFromWorld.size(), result.clipFromWorld.begin());
    std::copy_n(
        glm::value_ptr(inverseProjection),
        result.inverseProjection.size(),
        result.inverseProjection.begin());
    std::copy_n(
        glm::value_ptr(viewToWorld), result.viewToWorld.size(), result.viewToWorld.begin());
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

    static constexpr std::array<std::uint8_t, 4> kWhiteMaterial{255, 255, 255, 255};
    const auto attachMaterialTexture = [](rhi::StaticMeshUpload& mesh,
                                          const assets::StaticMesh& asset) {
        const auto& data = asset.material.metallicRoughness;
        mesh.materialRgba = data ? std::span<const std::uint8_t>{data->rgba} : kWhiteMaterial;
        mesh.materialWidth = data ? data->width : 1u;
        mesh.materialHeight = data ? data->height : 1u;
    };

    if (rhi.staticIndirectEnabled()) {
        // Indirect mode packs geometry once through packStaticSceneDrawData; per-mesh uploads carry
        // only texture identity and pixels because RHI skips per-mesh vertex/index buffers.
        const auto packed = packStaticSceneDrawData(input);
        if (!packed) {
            return std::unexpected(packed.error());
        }

        std::vector<rhi::StaticMeshUpload> meshes;
        meshes.reserve(packed->materials.size());
        for (const auto& material : packed->materials) {
            const auto& asset = material.source;
            rhi::StaticMeshUpload upload{asset, {}, {}, asset->rgba, asset->width, asset->height};
            attachMaterialTexture(upload, *asset);
            meshes.push_back(upload);
        }

        std::vector<rhi::StaticInstanceDraw> draws;
        draws.reserve(packed->instances.size());
        std::vector<std::uint64_t> objectIds;
        objectIds.reserve(packed->instances.size());
        for (const auto& instance : packed->instances) {
            const auto& material = packed->materials[instance.materialIndex];
            rhi::StaticInstanceDraw draw{instance.meshIndex, instance.modelMatrix};
            draw.normalFromModel = instance.normalMatrix;
            draw.normalFromModel[3] = material.metallicFactor;
            draw.normalFromModel[7] = material.roughnessFactor;
            draw.baseColorFactor = material.baseColorFactor;
            draws.push_back(draw);
            objectIds.push_back(instance.objectId);
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

        const rhi::StaticIndirectGeometry geometry{
            sharedVertices, packed->indices, ranges, objectIds};
        if (auto replaced = rhi.replaceStaticScene(meshes, draws, verifyRollback, &geometry);
            !replaced) {
            return replaced;
        }
        return rhi.setFrameView(*frameView);
    }

    struct DirectMesh {
        std::shared_ptr<const assets::StaticMesh> asset;
        std::vector<rhi::StaticVertex> vertices;
        std::vector<std::uint32_t> indices;
    };
    const StaticDrawLimits limits{};
    std::vector<DirectMesh> directMeshes;
    std::vector<rhi::StaticInstanceDraw> draws;
    std::unordered_map<const assets::StaticMesh*, std::uint32_t> indices;
    draws.reserve(input.draws.size());
    for (const auto& draw : input.draws) {
        const auto& asset = draw.mesh;
        if (!asset) {
            return detail::invalidStaticGeometry("static draw has no mesh asset");
        }
        std::array<float, 12> normalFromModel{};
        if (enableGBuffer) {
            auto normalMatrix = detail::computeStaticNormalMatrix(draw.modelMatrix);
            if (!normalMatrix) {
                return std::unexpected(std::move(normalMatrix.error()));
            }
            normalFromModel = *normalMatrix;
            const auto& material = asset->material;
            if (!std::isfinite(material.metallicFactor) ||
                !std::isfinite(material.roughnessFactor) || !allFinite(material.baseColorFactor)) {
                return detail::invalidStaticGeometry("static mesh has non-finite material factors");
            }
            normalFromModel[3] = material.metallicFactor;
            normalFromModel[7] = material.roughnessFactor;
        } else if (!allFinite(draw.modelMatrix)) {
            return detail::invalidStaticGeometry("static draw has a non-finite transform");
        }

        const auto [entry, inserted] =
            indices.try_emplace(asset.get(), static_cast<std::uint32_t>(directMeshes.size()));
        if (inserted) {
            DirectMesh mesh{asset, {}, {}};
            auto range = detail::appendStaticMeshGeometry(
                mesh.vertices, mesh.indices, *asset, limits, enableGBuffer);
            if (!range) {
                return std::unexpected(std::move(range.error()));
            }
            directMeshes.push_back(std::move(mesh));
        }

        rhi::StaticInstanceDraw instance{entry->second, draw.modelMatrix};
        if (enableGBuffer) {
            instance.normalFromModel = normalFromModel;
            instance.baseColorFactor = asset->material.baseColorFactor;
        }
        draws.push_back(instance);
    }

    std::vector<rhi::StaticMeshUpload> meshes;
    meshes.reserve(directMeshes.size());
    for (const auto& mesh : directMeshes) {
        rhi::StaticMeshUpload upload{
            mesh.asset,
            mesh.vertices,
            mesh.indices,
            mesh.asset->rgba,
            mesh.asset->width,
            mesh.asset->height};
        if (enableGBuffer) {
            attachMaterialTexture(upload, *mesh.asset);
        }
        meshes.push_back(upload);
    }

    if (auto replaced = rhi.replaceStaticScene(meshes, draws, verifyRollback); !replaced) {
        return replaced;
    }
    return rhi.setFrameView(*frameView);
}

} // namespace ar::render
