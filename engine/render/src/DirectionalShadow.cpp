#include "render/DirectionalShadow.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "assets/StaticAssetCache.h"

namespace ar::render {
namespace {

bool allFinite(std::span<const float> values) noexcept {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}

core::Error invalidArg(const char* message) {
    return core::Error{
        core::ErrorCategory::InvalidArguments, core::ErrorStage::Validation, message, {}};
}

std::expected<glm::vec3, core::Error> normalizeDirection(const std::array<float, 3>& direction) {
    if (!allFinite(direction)) {
        return std::unexpected(invalidArg("directional light direction must be finite"));
    }
    const double lengthSquared = static_cast<double>(direction[0]) * direction[0] +
                                 static_cast<double>(direction[1]) * direction[1] +
                                 static_cast<double>(direction[2]) * direction[2];
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-12) {
        return std::unexpected(invalidArg("directional light direction must be non-zero"));
    }
    const double invLength = 1.0 / std::sqrt(lengthSquared);
    return glm::vec3{
        static_cast<float>(direction[0] * invLength),
        static_cast<float>(direction[1] * invLength),
        static_cast<float>(direction[2] * invLength),
    };
}

} // namespace

DirectionalLight defaultDirectionalLight() noexcept {
    constexpr double dx = -0.4;
    constexpr double dy = -0.85;
    constexpr double dz = -0.35;
    const double inv = 1.0 / std::sqrt(dx * dx + dy * dy + dz * dz);
    return DirectionalLight{
        {static_cast<float>(dx * inv), static_cast<float>(dy * inv), static_cast<float>(dz * inv)},
        {1.F, 1.F, 1.F},
        6.0F,
        true,
    };
}

std::expected<void, core::Error> validateDirectionalLight(const DirectionalLight& light) {
    if (auto normalized = normalizeDirection(light.direction); !normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    if (!allFinite(light.color) ||
        std::ranges::any_of(light.color, [](float channel) { return channel < 0.F; })) {
        return std::unexpected(
            invalidArg("directional light color components must be finite and non-negative"));
    }
    if (!std::isfinite(light.intensity) || light.intensity < 0.F) {
        return std::unexpected(
            invalidArg("directional light intensity must be finite and non-negative"));
    }
    return {};
}

std::expected<EffectiveDirectionalLight, core::Error> resolveDirectionalLight(
    const std::optional<DirectionalLight>& sceneLight, bool shadowPipelineEnabled) {
    if (!sceneLight) {
        if (!shadowPipelineEnabled) {
            return EffectiveDirectionalLight{
                true,
                {0.F, 0.F, -1.F},
                {1.F, 1.F, 1.F},
                6.0F,
                false,
            };
        }
        const DirectionalLight fallback = defaultDirectionalLight();
        return EffectiveDirectionalLight{
            false,
            fallback.direction,
            fallback.color,
            fallback.intensity,
            true,
        };
    }

    if (auto valid = validateDirectionalLight(*sceneLight); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    const auto normalized = normalizeDirection(sceneLight->direction);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    return EffectiveDirectionalLight{
        false,
        {normalized->x, normalized->y, normalized->z},
        sceneLight->color,
        sceneLight->intensity,
        shadowPipelineEnabled && sceneLight->castShadows,
    };
}

std::expected<SceneWorldBounds, core::Error>
computeSceneWorldBounds(const StaticRenderInput& input) {
    if (input.draws.empty()) {
        return SceneWorldBounds{{-1.F, -1.F, -1.F}, {1.F, 1.F, 1.F}};
    }

    glm::vec3 worldMin(std::numeric_limits<float>::infinity());
    glm::vec3 worldMax(-std::numeric_limits<float>::infinity());
    bool anyVertex = false;

    for (const auto& draw : input.draws) {
        if (!draw.mesh || draw.mesh->vertices.empty()) {
            return std::unexpected(invalidArg("scene draw requires a non-empty static mesh"));
        }
        if (!allFinite(draw.modelMatrix)) {
            return std::unexpected(invalidArg("scene draw model matrix must be finite"));
        }
        glm::vec3 localMin(std::numeric_limits<float>::infinity());
        glm::vec3 localMax(-std::numeric_limits<float>::infinity());
        for (const auto& vertex : draw.mesh->vertices) {
            if (!allFinite(vertex.position)) {
                return std::unexpected(invalidArg("static mesh vertex position must be finite"));
            }
            const glm::vec3 p(vertex.position[0], vertex.position[1], vertex.position[2]);
            localMin = glm::min(localMin, p);
            localMax = glm::max(localMax, p);
        }
        const glm::mat4 model = glm::make_mat4(draw.modelMatrix.data());
        for (int mask = 0; mask < 8; ++mask) {
            const glm::vec4 corner(
                (mask & 1) ? localMax.x : localMin.x,
                (mask & 2) ? localMax.y : localMin.y,
                (mask & 4) ? localMax.z : localMin.z,
                1.F);
            const glm::vec4 transformed = model * corner;
            if (!std::isfinite(transformed.x) || !std::isfinite(transformed.y) ||
                !std::isfinite(transformed.z)) {
                return std::unexpected(invalidArg("transformed scene bounds must be finite"));
            }
            const glm::vec3 point(transformed.x, transformed.y, transformed.z);
            worldMin = glm::min(worldMin, point);
            worldMax = glm::max(worldMax, point);
            anyVertex = true;
        }
    }

    if (!anyVertex) {
        return SceneWorldBounds{{-1.F, -1.F, -1.F}, {1.F, 1.F, 1.F}};
    }
    return SceneWorldBounds{
        {worldMin.x, worldMin.y, worldMin.z},
        {worldMax.x, worldMax.y, worldMax.z},
    };
}

std::expected<DirectionalShadowView, core::Error> buildDirectionalShadowView(
    const SceneWorldBounds& bounds,
    const DirectionalLight& light,
    const DirectionalShadowConfig& config) {
    if (!allFinite(bounds.min) || !allFinite(bounds.max)) {
        return std::unexpected(invalidArg("scene world bounds must be finite"));
    }
    for (std::size_t axis = 0; axis < 3u; ++axis) {
        if (bounds.min[axis] > bounds.max[axis]) {
            return std::unexpected(invalidArg("scene world bounds min must not exceed max"));
        }
    }
    if (auto valid = validateDirectionalLight(light); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (config.resolution < kMinShadowResolution || config.resolution > kMaxShadowResolution) {
        return std::unexpected(invalidArg("shadow map resolution must be within [64, 4096]"));
    }
    if (!std::isfinite(config.normalBias) || config.normalBias < 0.F ||
        !std::isfinite(config.depthBias) || config.depthBias < 0.F ||
        !std::isfinite(config.pcfRadiusTexels) || config.pcfRadiusTexels < 0.F) {
        return std::unexpected(
            invalidArg("shadow bias and PCF radius parameters must be finite and non-negative"));
    }

    const auto forwardResult = normalizeDirection(light.direction);
    if (!forwardResult) {
        return std::unexpected(std::move(forwardResult.error()));
    }
    const glm::vec3 forward = *forwardResult;
    const glm::vec3 bMin(bounds.min[0], bounds.min[1], bounds.min[2]);
    const glm::vec3 bMax(bounds.max[0], bounds.max[1], bounds.max[2]);
    const glm::vec3 center = 0.5F * (bMin + bMax);
    const glm::vec3 extent = bMax - bMin;
    const float maxExtent = std::max({extent.x, extent.y, extent.z});
    const float radius = std::max(0.5F * glm::length(extent), 0.5F);
    const glm::vec3 upRef = std::abs(glm::dot(forward, glm::vec3(0.F, 1.F, 0.F))) > 0.99F
                                ? glm::vec3(0.F, 0.F, -1.F)
                                : glm::vec3(0.F, 1.F, 0.F);
    const glm::vec3 lightEye = center - forward * (2.0F * radius);
    const glm::mat4 lightView = glm::lookAtRH(lightEye, center, upRef);

    float minX = std::numeric_limits<float>::infinity();
    float maxX = -std::numeric_limits<float>::infinity();
    float minY = std::numeric_limits<float>::infinity();
    float maxY = -std::numeric_limits<float>::infinity();
    float minZ = std::numeric_limits<float>::infinity();
    float maxZ = -std::numeric_limits<float>::infinity();
    for (int mask = 0; mask < 8; ++mask) {
        const glm::vec4 corner(
            (mask & 1) ? bMax.x : bMin.x,
            (mask & 2) ? bMax.y : bMin.y,
            (mask & 4) ? bMax.z : bMin.z,
            1.F);
        const glm::vec4 viewCorner = lightView * corner;
        minX = std::min(minX, viewCorner.x);
        maxX = std::max(maxX, viewCorner.x);
        minY = std::min(minY, viewCorner.y);
        maxY = std::max(maxY, viewCorner.y);
        minZ = std::min(minZ, viewCorner.z);
        maxZ = std::max(maxZ, viewCorner.z);
    }

    const float margin = std::max(0.25F, 0.05F * maxExtent);
    const float left = minX - margin;
    const float right = maxX + margin;
    const float bottom = minY - margin;
    const float top = maxY + margin;
    const float zNear = std::max(0.1F, -maxZ - margin);
    const float zFar = std::max(zNear + 0.1F, -minZ + margin);

    const glm::mat4 lightProj = glm::orthoRH_ZO(left, right, bottom, top, zNear, zFar);
    const glm::mat4 lightViewProj = lightProj * lightView;

    DirectionalShadowView result{};
    std::copy_n(glm::value_ptr(lightView), result.lightView.size(), result.lightView.begin());
    std::copy_n(glm::value_ptr(lightProj), result.lightProj.size(), result.lightProj.begin());
    std::copy_n(
        glm::value_ptr(lightViewProj), result.lightViewProj.size(), result.lightViewProj.begin());
    result.lightEye = {lightEye.x, lightEye.y, lightEye.z};
    result.zNear = zNear;
    result.zFar = zFar;
    result.resolution = config.resolution;
    result.normalBias = config.normalBias;
    result.depthBias = config.depthBias;
    result.pcfRadiusTexels = config.pcfRadiusTexels;

    if (!allFinite(result.lightView) || !allFinite(result.lightProj) ||
        !allFinite(result.lightViewProj) || !allFinite(result.lightEye) ||
        !std::isfinite(result.zNear) || !std::isfinite(result.zFar)) {
        return std::unexpected(
            invalidArg("directional shadow parameters produced non-finite matrices"));
    }
    return result;
}

} // namespace ar::render
