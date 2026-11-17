#include "assets/StaticAssetCache.h"
#include "render/OpaqueMaterial.h"
#include "runtime/StaticSceneSession.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

namespace {
namespace fs = std::filesystem;

struct Fixture {
    fs::path directory;

    explicit Fixture(const fs::path& output)
        : directory(output / ("static-gltf-" + std::to_string(
                                     std::chrono::steady_clock::now().time_since_epoch().count()))) {
        fs::create_directories(directory / "assets/models");
    }

    ~Fixture() {
        std::error_code error;
        fs::remove_all(directory, error);
    }
};

template <typename T, std::size_t N>
void writeBytes(const fs::path& path, const std::array<T, N>& data, bool append = false) {
    std::ofstream out(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(T)));
    REQUIRE(out.good());
}

void writeModel(const fs::path& path, std::string buffer = "quad.bin",
                std::string image = "colors.png", std::string node = R"({"mesh":0})",
                std::string positionOffset = "0",
                std::string_view material = R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}})",
                std::string_view materialRef = R"(,"material":0)", unsigned normalCount = 0) {
    std::ofstream out(path, std::ios::binary);
    out << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[)"
        << node << R"(],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1)"
        << (normalCount ? R"(,"NORMAL":3)" : "") << "},"
        << R"("indices":2)" << materialRef << R"(,"mode":4}]}],"buffers":[{"uri":")" << buffer
        << R"(","byteLength":)" << (normalCount ? 140 : 92)
        << R"(}],"bufferViews":[{"buffer":0,"byteLength":48},)"
        << R"({"buffer":0,"byteOffset":48,"byteLength":32},{"buffer":0,"byteOffset":80,"byteLength":12})"
        << (normalCount ? R"(,{"buffer":0,"byteOffset":92,"byteLength":48})" : "") << "],"
        << R"("accessors":[{"bufferView":0,"byteOffset":)" << positionOffset
        << R"(,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
        << R"({"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},)"
        << R"({"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"})";
    if (normalCount) {
        out << R"(,{"bufferView":3,"componentType":5126,"type":"VEC3","count":)"
            << normalCount << '}';
    }
    out << R"(],"materials":[)" << material << R"(],)"
        << R"("textures":[{"source":0}],"images":[{"uri":")" << image << R"("}]})";
    REQUIRE(out.good());
}

fs::path testOutput() {
#ifdef _WIN32
    wchar_t output[4096]{};
    std::size_t length = 0;
    REQUIRE(_wgetenv_s(&length, output, std::size(output), L"AR_TEST_OUTPUT_ROOT") == 0);
    REQUIRE(length > 1);
#else
    const auto output = std::getenv("AR_TEST_OUTPUT_ROOT");
    REQUIRE(output != nullptr);
#endif
    return output;
}

void writeQuad(const fs::path& models) {
    // Two triangles share four vertices. PNG is a locally generated 2x1 RGBA red/blue fixture.
    const std::array positions{0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 1.F, 1.F, 0.F, 0.F, 1.F, 0.F};
    const std::array uv{0.F, 0.F, 1.F, 0.F, 1.F, 1.F, 0.F, 1.F};
    const std::array<std::uint16_t, 6> indices{0, 1, 2, 0, 2, 3};
    constexpr std::array<unsigned char, 71> png{
        137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,1,8,6,0,0,0,244,34,127,138,
        0,0,0,14,73,68,65,84,120,156,99,248,207,192,0,66,255,1,15,249,3,253,133,17,153,118,
        0,0,0,0,73,69,78,68,174,66,96,130};
    writeBytes(models / "quad.bin", positions);
    writeBytes(models / "quad.bin", uv, true);
    writeBytes(models / "quad.bin", indices, true);
    writeBytes(models / "colors.png", png);
    writeModel(models / "quad.gltf");
}
} // namespace

TEST_CASE("Static glTF resolves external files and only caches complete assets", "[assets-static-gltf]") {
    const Fixture fixture(testOutput());
    const auto root = fixture.directory / "assets";
    const auto models = root / "models";
    writeQuad(models);
    ar::assets::StaticAssetCache cache(root);
    auto loaded = cache.load("models/quad.gltf");
    REQUIRE(loaded.has_value());
    CHECK((*loaded)->vertices.size() == 4);
    CHECK((*loaded)->vertices.at(2).position == std::array{1.F, 1.F, 0.F});
    CHECK((*loaded)->vertices.at(3).uv == std::array{0.F, 1.F});
    CHECK((*loaded)->indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
    CHECK((*loaded)->width == 2);
    CHECK((*loaded)->height == 1);
    CHECK((*loaded)->rgba == std::vector<std::uint8_t>{255, 0, 0, 255, 0, 0, 255, 255});
    CHECK(cache.load("models/./quad.gltf").value() == *loaded);

    const auto bad = models / "retry.gltf";
    const std::string badRef = "models/retry.gltf";
    using ar::assets::AssetStage;
    for (const auto& [buffer, image, node, offset, stage] : {
             std::tuple{"../../outside.bin", "colors.png", R"({"mesh":0})", "0", AssetStage::Resolve},
             std::tuple{"quad.bin", "../../outside.png", R"({"mesh":0})", "0", AssetStage::Resolve},
             std::tuple{"missing.bin", "colors.png", R"({"mesh":0})", "0", AssetStage::Read},
             std::tuple{"quad.bin", "quad.png", R"({"mesh":0})", "0", AssetStage::Decode},
             std::tuple{"quad.bin", "colors.png", R"({"mesh":0,"translation":[1,0,0]})", "0", AssetStage::Geometry},
             std::tuple{"quad.bin", "colors.png", R"({"mesh":0})", "4", AssetStage::Geometry}}) {
        writeBytes(models / "quad.png", std::array{std::uint8_t{0}});
        writeModel(bad, buffer, image, node, offset);
        auto failure = cache.load(badRef);
        REQUIRE_FALSE(failure.has_value());
        CHECK(failure.error().stage == stage);
        CHECK_FALSE(failure.error().source.empty());
        CHECK_FALSE(failure.error().message.empty());
    }
    writeModel(bad);
    CHECK(cache.load(badRef).has_value());
}


TEST_CASE("Opaque glTF keeps factors and texture roles", "[assets-opaque-material]") {
    const Fixture fixture(testOutput());
    const auto root = fixture.directory / "assets";
    const auto models = root / "models";
    writeQuad(models);
    using ar::assets::StaticMaterialMode;
    ar::assets::StaticAssetCache cache(root, StaticMaterialMode::Opaque);
    const auto missingTexture = models / "default.gltf";
    writeModel(missingTexture, "quad.bin", "missing.png", R"({"mesh":0})", "0", "{}");
    const auto defaults = cache.load("models/default.gltf");
    REQUIRE(defaults.has_value());
    CHECK((*defaults)->normals.empty());
    CHECK((*defaults)->material.baseColorFactor == std::array{1.F, 1.F, 1.F, 1.F});
    CHECK((*defaults)->material.metallicFactor == 1.F);
    CHECK((*defaults)->material.roughnessFactor == 1.F);
    CHECK_FALSE((*defaults)->material.metallicRoughness);
    CHECK((*defaults)->width == 1);
    CHECK((*defaults)->height == 1);
    CHECK((*defaults)->rgba == std::vector<std::uint8_t>{255, 255, 255, 255});
    CHECK((*defaults)->baseColorSource.empty());
    writeModel(models / "no-material.gltf", "quad.bin", "missing.png", R"({"mesh":0})", "0", "{}", "");
    const auto noMaterial = cache.load("models/no-material.gltf");
    REQUIRE(noMaterial.has_value());
    CHECK((*noMaterial)->rgba == (*defaults)->rgba);

    constexpr std::array normals{0.F, 0.6F, 0.8F, 0.F, 0.6F, 0.8F,
                                 0.F, 0.6F, 0.8F, 0.F, 0.6F, 0.8F};
    writeBytes(models / "quad.bin", normals, true);
    writeModel(models / "normals.gltf", "quad.bin", "colors.png", R"({"mesh":0})", "0",
               "{}", R"(,"material":0)", 4);
    const auto withNormals = cache.load("models/normals.gltf");
    REQUIRE(withNormals.has_value());
    REQUIRE((*withNormals)->normals.size() == 4);
    CHECK((*withNormals)->normals[0] == std::array{0.F, 0.6F, 0.8F});
    writeModel(models / "bad-normals.gltf", "quad.bin", "colors.png", R"({"mesh":0})", "0",
               "{}", R"(,"material":0)", 3);
    const auto badNormals = cache.load("models/bad-normals.gltf");
    REQUIRE_FALSE(badNormals.has_value());
    CHECK(badNormals.error().stage == ar::assets::AssetStage::Geometry);

    // Non-endpoint RGB/alpha and distinct G/B detect color decoding and swapped channels.
    constexpr std::array<std::uint8_t, 70> png{
        137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0,
        0, 1, 8, 6, 0, 0, 0, 31, 21, 196, 137, 0, 0, 0, 13, 73, 68, 65, 84, 120, 156,
        99, 104, 112, 56, 176, 0, 0, 4, 229, 2, 33, 24, 57, 169, 78, 0, 0, 0, 0, 73,
        69, 78, 68, 174, 66, 96, 130};
    writeBytes(models / "material.png", png);
    const auto explicitPath = models / "explicit.gltf";
    constexpr std::string_view material = R"({"pbrMetallicRoughness":{"baseColorFactor":[0.5,0.6,0.7,0.8],"metallicFactor":0.4,"roughnessFactor":0.7,"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":0}}})";
    writeModel(explicitPath, "quad.bin", "material.png", R"({"mesh":0})", "0", material);
    const auto mesh = cache.load("models/explicit.gltf");
    REQUIRE(mesh.has_value());
    const auto& values = (*mesh)->material;
    CHECK(values.baseColorFactor == std::array{0.5F, 0.6F, 0.7F, 0.8F});
    CHECK(values.metallicFactor == 0.4F);
    CHECK(values.roughnessFactor == 0.7F);
    REQUIRE(values.metallicRoughness);
    CHECK(values.metallicRoughness->width == 1);
    CHECK(values.metallicRoughness->height == 1);
    CHECK((*mesh)->rgba == std::vector<std::uint8_t>{128, 64, 192, 160});
    CHECK(values.metallicRoughness->rgba == (*mesh)->rgba);
    CHECK_FALSE((*mesh)->baseColorSource.empty());
    CHECK((*mesh)->baseColorSource == values.metallicRoughness->source);
    const auto texel = [](const std::vector<std::uint8_t>& px) {
        REQUIRE(px.size() == 4);
        return std::array{px[0], px[1], px[2], px[3]};
    };
    const auto evaluated = ar::render::evaluateOpaqueMaterial(
        values, texel((*mesh)->rgba), texel(values.metallicRoughness->rgba));
    CHECK(evaluated.baseColor[0] == Catch::Approx(0.10793025));
    CHECK(evaluated.baseColor[1] == Catch::Approx(0.03076168));
    CHECK(evaluated.baseColor[2] == Catch::Approx(0.36898059));
    CHECK(evaluated.baseColor[3] == Catch::Approx(0.50196078));
    CHECK(evaluated.metallic == Catch::Approx(0.30117647));
    CHECK(evaluated.roughness == Catch::Approx(0.17568627));
    const auto white = ar::render::evaluateOpaqueMaterial(values);
    CHECK(white.baseColor == values.baseColorFactor);
    CHECK(white.metallic == values.metallicFactor);
    CHECK(white.roughness == values.roughnessFactor);

    const auto bad = models / "bad-material.gltf";
    for (const auto invalid : {R"({"pbrMetallicRoughness":{"metallicFactor":1.2}})",
                               R"({"pbrMetallicRoughness":{"baseColorFactor":[1,1,-0.5,1]}})",
                               R"({"pbrMetallicRoughness":{"metallicRoughnessTexture":{"index":5}}})",
                               R"({"alphaMode":"BLEND"})"}) {
        writeModel(bad, "quad.bin", "material.png", R"({"mesh":0})", "0", invalid);
        const auto result = cache.load("models/bad-material.gltf");
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().source.empty());
        CHECK_FALSE(result.error().message.empty());
    }
    writeModel(bad, "quad.bin", "missing.png", R"({"mesh":0})", "0",
               R"({"pbrMetallicRoughness":{"metallicRoughnessTexture":{"index":0}}})");
    CHECK_FALSE(cache.load("models/bad-material.gltf").has_value());
    writeModel(bad, "quad.bin", "material.png", R"({"mesh":0})", "0", material, R"(,"material":4)");
    CHECK_FALSE(cache.load("models/bad-material.gltf").has_value());
    ar::assets::StaticAssetCache legacy(root);
    writeModel(bad, "quad.bin", "material.png", R"({"mesh":0})", "0",
               R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":5}}})");
    REQUIRE(legacy.load("models/bad-material.gltf").has_value());
    CHECK_FALSE(cache.load("models/bad-material.gltf").has_value());
    writeModel(bad, "quad.bin", "material.png", R"({"mesh":0})", "0", material);
    CHECK(cache.load("models/bad-material.gltf").has_value());
}

TEST_CASE("Static scene keeps the active input until a candidate fully loads", "[scene-static-roundtrip]") {
    const Fixture fixture(testOutput());
    const auto root = fixture.directory / "assets";
    writeQuad(root / "models");
    const auto source = fixture.directory / "source.json";
    const auto saved = fixture.directory / "saved.json";
    const auto camera = R"({"position":[0,0,5],"orientation":[0,0,0,1],"fov_y":1,"near":0.25,"far":40})";
    const auto objects = R"([{"id":9,"asset":"models/quad.gltf","position":[2,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},{"id":3,"asset":"models/quad.gltf","position":[-2,0,0],"rotation":[0,0,0,1],"scale":[2,1,1]}])";
    const auto writeScene = [&](int version, std::string_view instances) {
        std::ofstream out(source);
        out << "{\"version\":" << version << ",\"camera\":" << camera << ",\"objects\":" << instances << '}';
        REQUIRE(out.good());
    };
    writeScene(1, objects);
    std::shared_ptr<const ar::render::StaticRenderInput> retained;
    {
        ar::runtime::StaticSceneSession session(root);
        REQUIRE(session.open(source).has_value());
        retained = session.input();
        REQUIRE(retained);
        REQUIRE(retained->draws.size() == 2);
        CHECK(retained->draws[0].objectId == 3);
        CHECK(retained->draws[1].objectId == 9);
        CHECK(retained->draws[0].mesh == retained->draws[1].mesh);
        CHECK(retained->draws[0].modelMatrix[0] == 2.F);
        CHECK(retained->draws[0].modelMatrix[12] == -2.F);
        CHECK(retained->draws[1].modelMatrix[12] == 2.F);
        CHECK(retained->camera.position[2] == 5.F);
        CHECK(retained->camera.nearPlane == 0.25F);
        REQUIRE(session.save(saved).has_value());
        REQUIRE(session.save(saved).has_value());
        REQUIRE(session.open(saved).has_value());
        const auto reopened = session.input();
        REQUIRE(reopened->draws.size() == 2);
        CHECK(reopened->camera == retained->camera);
        for (std::size_t i = 0; i < reopened->draws.size(); ++i) {
            CHECK(reopened->draws[i].objectId == retained->draws[i].objectId);
            CHECK(reopened->draws[i].modelMatrix == retained->draws[i].modelMatrix);
            CHECK(reopened->draws[i].mesh == retained->draws[i].mesh);
        }
        writeScene(2, objects);
        CHECK_FALSE(session.open(source).has_value());
        CHECK(session.input() == reopened);
        auto duplicate = std::string(objects);
        duplicate.replace(duplicate.find("\"id\":9") + 5, 1, "3");
        writeScene(1, duplicate);
        CHECK_FALSE(session.open(source).has_value());
        CHECK(session.input() == reopened);
        auto missing = std::string(objects);
        missing.replace(missing.find("quad.gltf"), 9, "missing.gltf");
        writeScene(1, missing);
        CHECK_FALSE(session.open(source).has_value());
        CHECK(session.input() == reopened);
    }
    REQUIRE(retained->draws[0].mesh);
    CHECK(retained->draws[0].mesh->indices.size() == 6);
}
