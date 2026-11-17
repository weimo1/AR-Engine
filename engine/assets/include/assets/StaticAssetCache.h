#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ar::assets {

struct StaticVertex {
    std::array<float, 3> position;
    std::array<float, 2> uv;
};

struct TexturePixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    std::filesystem::path source;
};

struct OpaqueMaterial {
    std::array<float, 4> baseColorFactor{1.F, 1.F, 1.F, 1.F};
    float metallicFactor = 1.F;
    float roughnessFactor = 1.F;
    // Raw linear data. G is roughness, B is metallic.
    std::optional<TexturePixels> metallicRoughness;
};

struct StaticMesh {
    std::vector<StaticVertex> vertices;
    std::vector<std::uint32_t> indices;
    // In opaque mode, empty means that the geometry stage supplies a face normal.
    std::vector<std::array<float, 3>> normals;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    // Empty for the opaque mode's implicit white texture.
    std::filesystem::path baseColorSource;
    OpaqueMaterial material;
};

enum class AssetStage { Resolve, Read, Parse, Geometry, Decode, Material };
enum class StaticMaterialMode { BaseColorOnly, Opaque };

struct AssetError {
    AssetStage stage;
    std::filesystem::path source;
    std::string message;
};

class StaticAssetCache {
public:
    explicit StaticAssetCache(
        std::filesystem::path root,
        StaticMaterialMode materialMode = StaticMaterialMode::BaseColorOnly);
    std::expected<std::shared_ptr<const StaticMesh>, AssetError>
    load(const std::filesystem::path& reference);

private:
    std::filesystem::path m_root;
    StaticMaterialMode m_materialMode;
    std::map<std::filesystem::path, std::shared_ptr<const StaticMesh>> m_meshes;
};

} // namespace ar::assets
