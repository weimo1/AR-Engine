#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ar::assets {

struct StaticVertex {
    std::array<float, 3> position;
    std::array<float, 2> uv;
};

struct StaticMesh {
    std::vector<StaticVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

enum class AssetStage { Resolve, Read, Parse, Geometry, Decode };

struct AssetError {
    AssetStage stage;
    std::filesystem::path source;
    std::string message;
};

class StaticAssetCache {
public:
    explicit StaticAssetCache(std::filesystem::path root);
    std::expected<std::shared_ptr<const StaticMesh>, AssetError>
    load(const std::filesystem::path& reference);

private:
    std::filesystem::path m_root;
    std::map<std::filesystem::path, std::shared_ptr<const StaticMesh>> m_meshes;
};

} // namespace ar::assets
