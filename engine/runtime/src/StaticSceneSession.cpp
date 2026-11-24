#include "runtime/StaticSceneSession.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "assets/StaticAssetCache.h"
#include "scene/StaticScene.h"

namespace ar::runtime {
namespace {

StaticSceneError sceneError(scene::FileError error) {
    using enum scene::FileStage;
    StaticSceneStage stage = StaticSceneStage::Validate;
    switch (error.stage) {
    case Read: stage = StaticSceneStage::Read; break;
    case Parse: stage = StaticSceneStage::Parse; break;
    case Validate: break;
    case Save: stage = StaticSceneStage::Save; break;
    }
    return {stage, std::move(error.source), std::move(error.message)};
}

std::array<float, 16> modelMatrix(const scene::Transform& transform) {
    const auto& [px, py, pz] = transform.position;
    const auto& [x, y, z, w] = transform.rotation;
    const auto& [sx, sy, sz] = transform.scale;
    const auto rotation = glm::normalize(glm::quat(w, x, y, z));
    const auto matrix = glm::translate(glm::mat4(1.F), {px, py, pz}) *
                        glm::mat4_cast(rotation) *
                        glm::scale(glm::mat4(1.F), {sx, sy, sz});
    std::array<float, 16> result{};
    std::copy_n(glm::value_ptr(matrix), result.size(), result.begin());
    return result;
}
} // namespace

struct StaticSceneSession::State {
    struct Active {
        scene::StaticScene scene;
        std::shared_ptr<const render::StaticRenderInput> input;
    };

    State(std::filesystem::path assetRoot, bool opaqueMaterials)
        : assets(std::move(assetRoot), opaqueMaterials ? assets::StaticMaterialMode::Opaque
                                                       : assets::StaticMaterialMode::BaseColorOnly) {}

    assets::StaticAssetCache assets;
    std::unique_ptr<Active> active;
};

StaticSceneSession::StaticSceneSession(std::filesystem::path assetRoot, bool opaqueMaterials)
    : m_state(std::make_unique<State>(std::move(assetRoot), opaqueMaterials)) {}
StaticSceneSession::~StaticSceneSession() = default;

std::expected<void, StaticSceneError> StaticSceneSession::open(
    const std::filesystem::path& source, const PrepareRender& prepareRender) {
    auto candidate = scene::StaticScene::load(source);
    if (!candidate) {
        return std::unexpected(sceneError(std::move(candidate.error())));
    }
    const auto camera = candidate->camera();
    render::StaticRenderInput input{
        {camera.position, camera.orientation, camera.verticalFovRadians, camera.nearPlane,
         camera.farPlane},
        {},
        std::nullopt};
    if (const auto light = candidate->directionalLight()) {
        input.directionalLight = render::DirectionalLight{
            light->direction, light->color, light->intensity, light->castShadows};
    }
    const auto instances = candidate->instances();
    input.draws.reserve(instances.size());
    // Deduplicate asset resolution within one open() so repeated instances avoid redundant
    // canonical path syscalls while keeping full root validation on each distinct asset reference.
    std::unordered_map<std::u8string, std::shared_ptr<const assets::StaticMesh>> loadedMeshes;
    for (const auto& instance : instances) {
        const auto model = modelMatrix(instance.transform);
        if (!std::ranges::all_of(model, [](float value) { return std::isfinite(value); })) {
            return std::unexpected(
                StaticSceneError{
                    StaticSceneStage::Validate,
                    source,
                    "non-finite model matrix for object " + std::to_string(instance.id)});
        }
        const auto assetKey = instance.asset.generic_u8string();
        std::shared_ptr<const assets::StaticMesh> mesh;
        if (const auto cached = loadedMeshes.find(assetKey); cached != loadedMeshes.end()) {
            mesh = cached->second;
        } else {
            auto loaded = m_state->assets.load(instance.asset);
            if (!loaded) {
                return std::unexpected(
                    StaticSceneError{
                        StaticSceneStage::Asset,
                        loaded.error().source,
                        "object " + std::to_string(instance.id) + ": " + loaded.error().message});
            }
            mesh = std::move(*loaded);
            loadedMeshes.emplace(assetKey, mesh);
        }
        input.draws.push_back({instance.id, model, std::move(mesh)});
    }
    auto ready = std::make_unique<State::Active>(State::Active{
        std::move(*candidate), std::make_shared<const render::StaticRenderInput>(std::move(input))});
    if (prepareRender) {
        if (auto prepared = prepareRender(*ready->input); !prepared) {
            return prepared;
        }
    }
    m_state->active = std::move(ready);
    return {};
}

std::expected<void, StaticSceneError>
StaticSceneSession::save(const std::filesystem::path& destination) const {
    if (!m_state->active) {
        return std::unexpected(StaticSceneError{
            StaticSceneStage::Save, destination, "there is no active scene to save"});
    }
    if (auto saved = m_state->active->scene.save(destination); !saved) {
        return std::unexpected(sceneError(std::move(saved.error())));
    }
    return {};
}

std::shared_ptr<const render::StaticRenderInput> StaticSceneSession::input() const {
    return m_state->active ? m_state->active->input : nullptr;
}

} // namespace ar::runtime
