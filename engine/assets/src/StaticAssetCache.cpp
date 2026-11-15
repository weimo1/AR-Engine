#include "assets/StaticAssetCache.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <span>
#include <utility>

#include <fastgltf/core.hpp>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace ar::assets {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t maxGltfBytes = 4 * 1024 * 1024;
constexpr std::size_t maxExternalBytes = 64 * 1024 * 1024;
constexpr std::size_t maxElements = 1024 * 1024;
constexpr int maxImageDimension = 4096;
static_assert(std::endian::native == std::endian::little && sizeof(float) == 4);

std::unexpected<AssetError> failure(AssetStage stage, const fs::path& source, std::string message) {
    return std::unexpected(AssetError{stage, source, std::move(message)});
}

std::expected<fs::path, AssetError>
resolve(const fs::path& root, const fs::path& directory, const fs::path& reference) {
    const auto& native = reference.native();
    if (reference.empty() || reference.has_root_path() ||
        native.find(static_cast<fs::path::value_type>(':')) != fs::path::string_type::npos ||
        native.find(fs::path::value_type{}) != fs::path::string_type::npos) {
        return failure(AssetStage::Resolve, reference, "expected a local relative asset path");
    }
    std::error_code error;
    auto path = fs::weakly_canonical(directory / reference, error);
    if (error) {
        return failure(AssetStage::Resolve, reference, "could not resolve asset path");
    }
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative == "." || *relative.begin() == "..") {
        return failure(AssetStage::Resolve, reference, "asset path is outside its root");
    }
    return path;
}

std::expected<fs::path, AssetError> resolveExternal(
    const fs::path& root, const fs::path& model, const fastgltf::DataSource& source,
    const fs::path& extension) {
    const auto* uri = std::get_if<fastgltf::sources::URI>(&source);
    if (!uri || !uri->uri.isLocalPath() || uri->fileByteOffset != 0) {
        return failure(AssetStage::Resolve, model, "expected an external file URI");
    }
    const auto reference = uri->uri.fspath();
    if (reference.extension() != extension) {
        return failure(AssetStage::Resolve, reference, "unsupported external file extension");
    }
    return resolve(root, model.parent_path(), reference);
}

std::expected<std::vector<std::byte>, AssetError> readFile(
    const fs::path& file, std::size_t limit) {
    std::error_code error;
    if (!fs::is_regular_file(file, error)) {
        return failure(AssetStage::Read, file, "asset file is missing or is not a regular file");
    }
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > limit) {
        return failure(AssetStage::Read, file, "could not read asset size or file exceeds byte limit");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()))) {
        return failure(AssetStage::Read, file, "asset read did not complete");
    }
    return bytes;
}

bool identityNode(const fastgltf::Node& node) {
    const auto* trs = std::get_if<fastgltf::TRS>(&node.transform);
    return trs && trs->translation == fastgltf::math::fvec3(0.F) &&
           trs->rotation == fastgltf::math::fquat(0.F, 0.F, 0.F, 1.F) &&
           trs->scale == fastgltf::math::fvec3(1.F) && node.children.empty() &&
           !node.skinIndex && !node.cameraIndex && node.meshIndex && *node.meshIndex == 0;
}

std::span<const std::byte> elements(
    const fastgltf::Asset& asset, const fastgltf::Accessor& accessor,
    std::span<const std::byte> buffer, std::size_t elementSize, std::size_t alignment) {
    if (!accessor.bufferViewIndex || *accessor.bufferViewIndex >= asset.bufferViews.size() ||
        accessor.normalized || accessor.sparse || accessor.count == 0 ||
        accessor.count > maxElements) {
        return {};
    }
    const auto& view = asset.bufferViews[*accessor.bufferViewIndex];
    if (view.bufferIndex != 0 || view.byteStride || view.byteOffset > buffer.size() ||
        view.byteLength > buffer.size() - view.byteOffset ||
        accessor.byteOffset > view.byteLength ||
        accessor.count > (view.byteLength - accessor.byteOffset) / elementSize ||
        accessor.byteOffset % alignment != 0 || view.byteOffset % alignment != 0) {
        return {};
    }
    return buffer.subspan(view.byteOffset + accessor.byteOffset, accessor.count * elementSize);
}

std::expected<void, AssetError> loadGeometry(
    StaticMesh& mesh, const fastgltf::Asset& asset, const fastgltf::Primitive& primitive,
    std::span<const std::byte> buffer, const fs::path& path) {
    const auto positionAttr = primitive.findAttribute("POSITION");
    const auto uvAttr = primitive.findAttribute("TEXCOORD_0");
    if (!primitive.indicesAccessor || positionAttr == primitive.attributes.end() ||
        uvAttr == primitive.attributes.end() || *primitive.indicesAccessor >= asset.accessors.size() ||
        positionAttr->accessorIndex >= asset.accessors.size() ||
        uvAttr->accessorIndex >= asset.accessors.size()) {
        return failure(AssetStage::Geometry, path, "missing position, UV or index accessor");
    }
    const auto& positions = asset.accessors[positionAttr->accessorIndex];
    const auto& uvs = asset.accessors[uvAttr->accessorIndex];
    const auto& indices = asset.accessors[*primitive.indicesAccessor];
    using enum fastgltf::ComponentType;
    if (positions.type != fastgltf::AccessorType::Vec3 || positions.componentType != Float ||
        uvs.type != fastgltf::AccessorType::Vec2 || uvs.componentType != Float ||
        positions.count != uvs.count || indices.type != fastgltf::AccessorType::Scalar ||
        indices.count % 3 != 0 ||
        (indices.componentType != UnsignedByte && indices.componentType != UnsignedShort &&
         indices.componentType != UnsignedInt)) {
        return failure(AssetStage::Geometry, path, "unsupported position, UV or index layout");
    }
    const auto indexWidth = fastgltf::getComponentByteSize(indices.componentType);
    constexpr std::size_t positionWidth = 3 * sizeof(float);
    constexpr std::size_t uvWidth = 2 * sizeof(float);
    const auto positionBytes = elements(asset, positions, buffer, positionWidth, sizeof(float));
    const auto uvBytes = elements(asset, uvs, buffer, uvWidth, sizeof(float));
    const auto indexBytes = elements(asset, indices, buffer, indexWidth, indexWidth);
    if (positionBytes.empty() || uvBytes.empty() || indexBytes.empty()) {
        return failure(AssetStage::Geometry, path, "accessor layout or buffer bounds are invalid");
    }
    mesh.vertices.resize(positions.count);
    for (std::size_t i = 0; i < positions.count; ++i) {
        auto& vertex = mesh.vertices[i];
        std::memcpy(vertex.position.data(), positionBytes.data() + i * positionWidth, positionWidth);
        std::memcpy(vertex.uv.data(), uvBytes.data() + i * uvWidth, uvWidth);
        for (const float coordinate : vertex.position) {
            if (!std::isfinite(coordinate)) {
                return failure(AssetStage::Geometry, path, "non-finite vertex position");
            }
        }
        for (const float coordinate : vertex.uv) {
            if (!std::isfinite(coordinate)) {
                return failure(AssetStage::Geometry, path, "non-finite vertex UV");
            }
        }
    }
    mesh.indices.resize(indices.count);
    for (std::size_t i = 0; i < indices.count; ++i) {
        std::memcpy(&mesh.indices[i], indexBytes.data() + i * indexWidth, indexWidth);
        if (mesh.indices[i] >= mesh.vertices.size()) {
            return failure(AssetStage::Geometry, path, "index exceeds vertex range");
        }
    }
    return {};
}

std::expected<void, AssetError> decodePng(StaticMesh& mesh, const fs::path& path) {
    auto bytes = readFile(path, maxExternalBytes);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes->data());
    const auto size = static_cast<int>(bytes->size());
    int width = 0;
    int height = 0;
    int channels = 0;
    if (bytes->empty() || !stbi_info_from_memory(data, size, &width, &height, &channels) ||
        width <= 0 || height <= 0 || width > maxImageDimension || height > maxImageDimension) {
        return failure(AssetStage::Decode, path, "invalid PNG header or dimensions");
    }
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> image(
        stbi_load_from_memory(data, size, &width, &height, &channels, STBI_rgb_alpha),
        stbi_image_free);
    if (!image) {
        return failure(AssetStage::Decode, path, "could not decode PNG pixels");
    }
    mesh.width = static_cast<std::uint32_t>(width);
    mesh.height = static_cast<std::uint32_t>(height);
    mesh.rgba.assign(image.get(), image.get() + static_cast<std::size_t>(width) * height * 4);
    return {};
}

std::expected<StaticMesh, AssetError> loadMesh(const fs::path& root, const fs::path& model) {
    auto json = readFile(model, maxGltfBytes);
    if (!json) {
        return std::unexpected(json.error());
    }
    if (json->empty()) {
        return failure(AssetStage::Parse, model, "empty glTF document");
    }
    auto data = fastgltf::GltfDataBuffer::FromBytes(json->data(), json->size());
    if (data.error() != fastgltf::Error::None) {
        return failure(AssetStage::Parse, model, std::string(fastgltf::getErrorMessage(data.error())));
    }
    fastgltf::Parser parser;
    // Keep external sources as URIs so they pass root checks before any reads.
    auto parsed = parser.loadGltfJson(data.get(), model.parent_path());
    if (parsed.error() != fastgltf::Error::None) {
        return failure(AssetStage::Parse, model, std::string(fastgltf::getErrorMessage(parsed.error())));
    }
    const auto& asset = parsed.get();
    if (asset.scenes.size() != 1 || !asset.defaultScene || *asset.defaultScene != 0 ||
        asset.scenes.front().nodeIndices.size() != 1 || asset.scenes.front().nodeIndices.front() != 0 ||
        asset.nodes.size() != 1 || !identityNode(asset.nodes.front()) || asset.meshes.size() != 1 ||
        asset.meshes.front().primitives.size() != 1 || !asset.extensionsUsed.empty() ||
        !asset.extensionsRequired.empty() || !asset.animations.empty() || !asset.skins.empty() ||
        !asset.nodes.front().weights.empty() || !asset.meshes.front().weights.empty() ||
        asset.buffers.size() != 1) {
        return failure(AssetStage::Geometry, model, "unsupported static scene, node or mesh layout");
    }
    const auto& primitive = asset.meshes.front().primitives.front();
    if (primitive.type != fastgltf::PrimitiveType::Triangles || !primitive.targets.empty() ||
        !primitive.materialIndex || *primitive.materialIndex >= asset.materials.size()) {
        return failure(AssetStage::Geometry, model, "expected TRIANGLES with a base-color material");
    }
    const auto& baseColor = asset.materials[*primitive.materialIndex].pbrData.baseColorTexture;
    if (!baseColor || baseColor->texCoordIndex != 0 || baseColor->textureIndex >= asset.textures.size()) {
        return failure(AssetStage::Decode, model, "base-color texture must use TEXCOORD_0");
    }
    const auto& texture = asset.textures[baseColor->textureIndex];
    if (!texture.imageIndex || *texture.imageIndex >= asset.images.size()) {
        return failure(AssetStage::Decode, model, "base-color texture image is missing");
    }
    const auto& source = asset.buffers.front();
    auto bin = resolveExternal(root, model, source.data, ".bin");
    if (!bin) {
        return std::unexpected(bin.error());
    }
    auto png = resolveExternal(root, model, asset.images[*texture.imageIndex].data, ".png");
    if (!png) {
        return std::unexpected(png.error());
    }
    auto bytes = readFile(*bin, maxExternalBytes);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    if (source.byteLength > bytes->size()) {
        return failure(AssetStage::Geometry, *bin, "buffer is shorter than its declared length");
    }
    StaticMesh mesh;
    if (auto geometry = loadGeometry(mesh, asset, primitive,
                                     std::span(*bytes).first(source.byteLength), model);
        !geometry) {
        return std::unexpected(geometry.error());
    }
    if (auto image = decodePng(mesh, *png); !image) {
        return std::unexpected(image.error());
    }
    return mesh;
}
} // namespace

StaticAssetCache::StaticAssetCache(std::filesystem::path root) : m_root(std::move(root)) {}

std::expected<std::shared_ptr<const StaticMesh>, AssetError>
StaticAssetCache::load(const std::filesystem::path& reference) {
    std::error_code error;
    const auto root = fs::canonical(m_root, error);
    if (error || !fs::is_directory(root, error)) {
        return failure(AssetStage::Resolve, m_root, "asset root is not an accessible directory");
    }
    if (reference.extension() != ".gltf") {
        return failure(AssetStage::Resolve, reference, "expected a .gltf reference");
    }
    auto path = resolve(root, root, reference);
    if (!path) {
        return std::unexpected(path.error());
    }
    if (const auto cached = m_meshes.find(*path); cached != m_meshes.end()) {
        return cached->second;
    }
    auto loaded = loadMesh(root, *path);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    auto mesh = std::make_shared<const StaticMesh>(std::move(*loaded));
    m_meshes.emplace(std::move(*path), mesh);
    return mesh;
}

} // namespace ar::assets
