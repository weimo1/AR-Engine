#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

#include "core/Error.h"
#include "render/StaticRenderInput.h"

namespace ar::render {

struct PackedStaticVertex {
    std::array<float, 3> position;
    std::array<float, 2> uv;
    std::array<float, 3> normal;
};

struct StaticMeshRange {
    std::uint32_t firstIndex;
    std::uint32_t indexCount;
    std::int32_t vertexOffset;
    std::array<float, 3> minimum;
    std::array<float, 3> maximum;
};

struct StaticDrawInstance {
    std::uint64_t objectId;
    std::uint32_t meshIndex;
    std::uint32_t materialIndex;
    std::array<float, 16> modelMatrix;
    std::array<float, 12> normalMatrix;
};

struct StaticDrawMaterial {
    std::array<float, 4> baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
    // Retains both source images until the binding path has uploaded them.
    std::shared_ptr<const assets::StaticMesh> source;
};

struct StaticSceneDrawData {
    std::vector<PackedStaticVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<StaticMeshRange> meshes;
    std::vector<StaticDrawInstance> instances;
    std::vector<StaticDrawMaterial> materials;
};

struct StaticDrawLimits {
    std::uint32_t vertices = 0x7fffffff;
    std::uint32_t indices = 0xffffffff;
    std::uint32_t instances = 0xffffffff;
};

std::expected<StaticSceneDrawData, core::Error>
packStaticSceneDrawData(const StaticRenderInput& input, StaticDrawLimits limits = {});

} // namespace ar::render
