#include "render/StaticSceneRenderer.h"

#include <algorithm>
#include <unordered_map>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "assets/StaticAssetCache.h"
#include "rhi/RhiSession.h"

namespace ar::render {

std::expected<void, core::Error> publishStaticScene(
    rhi::RhiSession& rhi, const StaticRenderInput& input, bool verifyRollback) {
    const auto& camera = input.camera;
    const auto& [px, py, pz] = camera.position;
    const auto& [x, y, z, w] = camera.orientation;
    const auto orientation = glm::normalize(glm::quat(w, x, y, z));
    const auto view = glm::mat4_cast(glm::conjugate(orientation)) *
                      glm::translate(glm::mat4(1.F), {-px, -py, -pz});
    auto projection = glm::perspectiveRH_ZO(
        camera.verticalFovRadians, 1.F, camera.farPlane, camera.nearPlane);
    projection[1][1] *= -1.F;
    const auto clipFromWorld = projection * view;
    std::vector<std::vector<rhi::StaticVertex>> vertices;
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
            buffer.reserve(asset->vertices.size());
            for (const auto& vertex : asset->vertices) {
                buffer.push_back({vertex.position, vertex.uv});
            }
            meshes.push_back({asset, buffer, asset->indices, asset->rgba, asset->width, asset->height});
        }
        const auto clip = clipFromWorld * glm::make_mat4(draw.modelMatrix.data());
        auto& instance = draws.emplace_back(rhi::StaticInstanceDraw{entry->second, {}});
        std::copy_n(glm::value_ptr(clip), instance.clipFromModel.size(), instance.clipFromModel.begin());
    }
    return rhi.replaceStaticScene(meshes, draws, verifyRollback);
}

} // namespace ar::render
