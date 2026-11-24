#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ar::assets {
struct StaticMesh;
}

namespace ar::render {

struct StaticCamera {
    std::array<float, 3> position{};
    std::array<float, 4> orientation{0.F, 0.F, 0.F, 1.F};
    float verticalFovRadians = 0.785398163F;
    float nearPlane = 0.1F;
    float farPlane = 100.F;

    bool operator==(const StaticCamera&) const = default;
};

struct StaticDraw {
    std::uint64_t objectId;
    std::array<float, 16> modelMatrix;
    std::shared_ptr<const assets::StaticMesh> mesh;
};

struct DirectionalLight {
    std::array<float, 3> direction{-0.4F, -0.85F, -0.35F};
    std::array<float, 3> color{1.F, 1.F, 1.F};
    float intensity = 6.0F;
    bool castShadows = true;

    bool operator==(const DirectionalLight&) const = default;
};

struct StaticRenderInput {
    StaticCamera camera;
    std::vector<StaticDraw> draws;
    std::optional<DirectionalLight> directionalLight{};
};

} // namespace ar::render
