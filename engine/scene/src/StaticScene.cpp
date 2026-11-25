#include "scene/StaticScene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

namespace ar::scene {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::size_t maxSceneBytes = 32 * 1024 * 1024;
constexpr std::size_t maxInstances = 65536;

std::unexpected<FileError> fail(FileStage stage, const fs::path& source, std::string message) {
    return std::unexpected(FileError{stage, source, std::move(message)});
}

float number(const Json& value) {
    if (!value.is_number()) {
        throw std::invalid_argument("expected numeric value");
    }
    const auto result = value.get<float>();
    if (!std::isfinite(result)) {
        throw std::invalid_argument("number must be finite");
    }
    return result;
}

template <std::size_t N> std::array<float, N> vector(const Json& value) {
    if (!value.is_array() || value.size() != N) {
        throw std::invalid_argument("invalid vector length");
    }
    std::array<float, N> result{};
    std::ranges::transform(value, result.begin(), number);
    return result;
}

std::array<float, 4> quaternion(const Json& value) {
    const auto result = vector<4>(value);
    double lengthSquared = 0;
    for (const auto component : result) {
        lengthSquared += static_cast<double>(component) * component;
    }
    if (std::abs(lengthSquared - 1.0) > 0.001) {
        throw std::invalid_argument("quaternion must have unit length");
    }
    return result;
}

Camera readCamera(const Json& value) {
    Camera camera{
        vector<3>(value.at("position")),
        quaternion(value.at("orientation")),
        number(value.at("fov_y")),
        number(value.at("near")),
        number(value.at("far"))};
    if (camera.verticalFovRadians <= 0 || camera.verticalFovRadians >= std::numbers::pi_v<float> ||
        camera.nearPlane <= 0 || camera.nearPlane >= camera.farPlane) {
        throw std::invalid_argument("invalid camera field of view or clip planes");
    }
    return camera;
}

DirectionalLight readDirectionalLight(const Json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("directional_light must be a JSON object");
    }
    DirectionalLight light{};
    light.direction = vector<3>(value.at("direction"));
    const double lengthSquared = static_cast<double>(light.direction[0]) * light.direction[0] +
                                 static_cast<double>(light.direction[1]) * light.direction[1] +
                                 static_cast<double>(light.direction[2]) * light.direction[2];
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-12) {
        throw std::invalid_argument("directional_light direction must be finite and non-zero");
    }
    if (const auto it = value.find("color"); it != value.end()) {
        light.color = vector<3>(*it);
        for (const float channel : light.color) {
            if (channel < 0.F) {
                throw std::invalid_argument(
                    "directional_light color components must be non-negative");
            }
        }
    }
    if (const auto it = value.find("intensity"); it != value.end()) {
        light.intensity = number(*it);
        if (light.intensity < 0.F) {
            throw std::invalid_argument("directional_light intensity must be non-negative");
        }
    }
    if (const auto it = value.find("cast_shadows"); it != value.end()) {
        if (!it->is_boolean()) {
            throw std::invalid_argument("directional_light cast_shadows must be a boolean");
        }
        light.castShadows = it->get<bool>();
    }
    return light;
}

Instance readInstance(const Json& value) {
    const auto& id = value.at("id");
    if (!id.is_number_unsigned() || id.get<std::uint64_t>() == 0) {
        throw std::invalid_argument("object ID must be a positive integer");
    }
    const auto key = value.at("asset").get<std::string>();
    if (key.empty() || key.find_first_of(":\\") != std::string::npos ||
        key.find('\0') != std::string::npos) {
        throw std::invalid_argument("asset must be a relative path using forward slashes");
    }
    const fs::path reference(std::u8string(key.begin(), key.end()));
    auto asset = reference.lexically_normal();
    if (asset.has_root_path() || asset.empty() || *asset.begin() == ".." ||
        asset.extension() != ".gltf") {
        throw std::invalid_argument("invalid relative glTF asset path");
    }
    return {
        id.get<std::uint64_t>(),
        std::move(asset),
        {vector<3>(value.at("position")),
         quaternion(value.at("rotation")),
         vector<3>(value.at("scale"))}};
}

std::expected<void, FileError> saveFile(const fs::path& destination, const std::string& bytes) {
    static std::atomic<std::uint64_t> sequence{0};
    auto temp = destination;
    temp += ".ar-scene-" +
            std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    std::FILE* file = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&file, temp.c_str(), L"wbx") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(temp.c_str(), "wbx");
#endif
    if (!file) {
        return fail(FileStage::Save, destination, "could not create a temporary scene file");
    }
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = std::fclose(file) == 0;
    std::error_code error;
    if (written && closed) {
        fs::rename(temp, destination, error);
        if (!error) {
            return {};
        }
    }
    std::error_code ignored;
    fs::remove(temp, ignored);
    return fail(FileStage::Save, destination, "could not write or replace the scene file");
}
} // namespace

struct StaticScene::Data {
    entt::registry registry;
    Camera camera;
    std::optional<DirectionalLight> directionalLight;
};

StaticScene::StaticScene(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
StaticScene::~StaticScene() = default;
StaticScene::StaticScene(StaticScene&&) noexcept = default;
StaticScene& StaticScene::operator=(StaticScene&&) noexcept = default;

std::expected<StaticScene, FileError> StaticScene::load(const fs::path& source) {
    std::error_code error;
    if (!fs::is_regular_file(source, error)) {
        return fail(FileStage::Read, source, "scene is missing or not a regular file");
    }
    std::ifstream stream(source, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > maxSceneBytes) {
        return fail(
            FileStage::Read, source, "could not read scene size or scene exceeds byte limit");
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        return fail(FileStage::Read, source, "could not read scene bytes");
    }
    auto document = Json::parse(bytes, nullptr, false);
    if (document.is_discarded()) {
        return fail(FileStage::Parse, source, "invalid scene JSON");
    }
    try {
        const auto& version = document.at("version");
        if (!version.is_number_integer() || version != 1) {
            return fail(FileStage::Validate, source, "unsupported scene version");
        }
        const auto& objects = document.at("objects");
        if (!objects.is_array() || objects.size() > maxInstances) {
            return fail(FileStage::Validate, source, "invalid object array or too many instances");
        }
        auto data = std::make_unique<Data>();
        data->camera = readCamera(document.at("camera"));
        if (const auto light = document.find("directional_light"); light != document.end()) {
            data->directionalLight = readDirectionalLight(*light);
        }
        std::unordered_set<std::uint64_t> ids;
        for (const auto& object : objects) {
            auto instance = readInstance(object);
            if (!ids.insert(instance.id).second) {
                return fail(
                    FileStage::Validate,
                    source,
                    "duplicate object ID " + std::to_string(instance.id));
            }
            const auto entity = data->registry.create();
            data->registry.emplace<Instance>(entity, std::move(instance));
        }
        return StaticScene(std::move(data));
    } catch (const Json::exception& exception) {
        return fail(FileStage::Validate, source, exception.what());
    } catch (const std::invalid_argument& exception) {
        return fail(FileStage::Validate, source, exception.what());
    } catch (const fs::filesystem_error& exception) {
        return fail(FileStage::Validate, source, exception.what());
    }
}

Camera StaticScene::camera() const {
    return m_data->camera;
}

std::optional<DirectionalLight> StaticScene::directionalLight() const {
    return m_data->directionalLight;
}

std::vector<Instance> StaticScene::instances() const {
    std::vector<Instance> result;
    const auto view = std::as_const(m_data->registry).view<const Instance>();
    result.reserve(view.size());
    for (const auto entity : view) {
        result.push_back(view.get<const Instance>(entity));
    }
    std::ranges::sort(result, {}, &Instance::id);
    return result;
}

std::expected<void, FileError> StaticScene::save(const fs::path& destination) const {
    const auto view = camera();
    Json objects = Json::array();
    for (const auto& instance : instances()) {
        const auto key = instance.asset.generic_u8string();
        objects.push_back(
            {{"id", instance.id},
             {"asset", std::string(key.begin(), key.end())},
             {"position", instance.transform.position},
             {"rotation", instance.transform.rotation},
             {"scale", instance.transform.scale}});
    }
    Json document{
        {"version", 1},
        {"objects", std::move(objects)},
        {"camera",
         {{"position", view.position},
          {"orientation", view.orientation},
          {"fov_y", view.verticalFovRadians},
          {"near", view.nearPlane},
          {"far", view.farPlane}}}};
    if (const auto light = directionalLight()) {
        document["directional_light"] = {
            {"direction", light->direction},
            {"color", light->color},
            {"intensity", light->intensity},
            {"cast_shadows", light->castShadows}};
    }
    auto bytes = document.dump(2) + '\n';
    if (bytes.size() > maxSceneBytes) {
        bytes = document.dump() + '\n';
    }
    if (bytes.size() > maxSceneBytes) {
        return fail(FileStage::Save, destination, "serialized scene exceeds byte limit");
    }
    return saveFile(destination, bytes);
}

} // namespace ar::scene
