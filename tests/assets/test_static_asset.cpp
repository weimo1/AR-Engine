#include "assets/StaticAssetCache.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>

#include <catch2/catch_test_macros.hpp>

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
                std::string positionOffset = "0") {
    std::ofstream out(path, std::ios::binary);
    out << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[)"
        << node << R"(],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},)"
        << R"("indices":2,"material":0,"mode":4}]}],"buffers":[{"uri":")" << buffer
        << R"(","byteLength":92}],"bufferViews":[{"buffer":0,"byteLength":48},)"
        << R"({"buffer":0,"byteOffset":48,"byteLength":32},{"buffer":0,"byteOffset":80,"byteLength":12}],)"
        << R"("accessors":[{"bufferView":0,"byteOffset":)" << positionOffset
        << R"(,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
        << R"({"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},)"
        << R"({"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"}],)"
        << R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)"
        << R"("textures":[{"source":0}],"images":[{"uri":")" << image << R"("}]})";
    REQUIRE(out.good());
}
} // namespace

TEST_CASE("Static glTF resolves external files and only caches complete assets", "[assets-static-gltf]") {
#ifdef _WIN32
    wchar_t output[4096]{};
    std::size_t length = 0;
    REQUIRE(_wgetenv_s(&length, output, std::size(output), L"AR_TEST_OUTPUT_ROOT") == 0);
    REQUIRE(length > 1);
#else
    const auto output = std::getenv("AR_TEST_OUTPUT_ROOT");
    REQUIRE(output != nullptr);
#endif
    const Fixture fixture(output);
    const auto root = fixture.directory / "assets";
    const auto models = root / "models";
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
