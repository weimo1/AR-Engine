#include "runtime/ObservationSettings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "render/DirectionalShadow.h"

namespace ar::runtime {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::size_t kMaxPresetBytes = 1024u * 1024u;
constexpr float kMaxExposureAbsEV = 16.F;

std::string pathToUtf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

core::Error makeError(core::ErrorCategory category, core::ErrorStage stage, std::string message) {
    return core::Error{category, stage, std::move(message), std::nullopt};
}

core::Error fileError(
    core::ErrorCategory category,
    core::ErrorStage stage,
    const fs::path& path,
    std::string_view reason) {
    return makeError(category, stage, pathToUtf8(path) + ": " + std::string(reason));
}

bool validRelativeSceneRef(std::string_view ref) {
    if (ref.empty() || ref.size() > 512u || ref.find_first_of(":\\") != std::string_view::npos ||
        ref.find('\0') != std::string_view::npos) {
        return false;
    }
    const fs::path raw(std::u8string(ref.begin(), ref.end()));
    const fs::path normalized = raw.lexically_normal();
    if (normalized.has_root_path() || normalized.empty()) {
        return false;
    }
    for (const auto& part : normalized) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

bool allFinite(std::span<const float> values) noexcept {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}

float readNumber(const Json& value, const char* field) {
    if (!value.is_number()) {
        throw std::invalid_argument(std::string(field) + " must be a number");
    }
    const float result = value.get<float>();
    if (!std::isfinite(result)) {
        throw std::invalid_argument(std::string(field) + " must be finite");
    }
    return result;
}

template <std::size_t N> std::array<float, N> readVector(const Json& value, const char* field) {
    if (!value.is_array() || value.size() != N) {
        throw std::invalid_argument(std::string(field) + " has invalid array length");
    }
    std::array<float, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = readNumber(value[i], field);
    }
    return out;
}

std::expected<void, core::Error>
atomicWriteFile(const fs::path& destination, const std::string& bytes) {
    if (destination.empty()) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Output,
            "preset destination path must not be empty"));
    }
    static std::atomic<std::uint64_t> sequence{0};
    fs::path temp = destination;
    temp += ".ar-preset-" +
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
    if (file == nullptr) {
        return std::unexpected(fileError(
            core::ErrorCategory::OutputFailed,
            core::ErrorStage::Output,
            destination,
            "could not create temporary observation preset file"));
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
    return std::unexpected(fileError(
        core::ErrorCategory::OutputFailed,
        core::ErrorStage::Output,
        destination,
        "could not write or replace observation preset file"));
}

} // namespace

std::string_view debugViewModeName(rhi::DebugViewMode mode) noexcept {
    switch (mode) {
    case rhi::DebugViewMode::Final:
        return "final";
    case rhi::DebugViewMode::BaseColor:
        return "base_color";
    case rhi::DebugViewMode::ViewNormal:
        return "view_normal";
    case rhi::DebugViewMode::MetallicRoughness:
        return "metallic_roughness";
    case rhi::DebugViewMode::MainDepth:
        return "main_depth";
    case rhi::DebugViewMode::ShadowDepth:
        return "shadow_depth";
    }
    return "unknown";
}

std::optional<rhi::DebugViewMode> parseDebugViewMode(std::string_view name) noexcept {
    if (name == "final") {
        return rhi::DebugViewMode::Final;
    }
    if (name == "base_color") {
        return rhi::DebugViewMode::BaseColor;
    }
    if (name == "view_normal") {
        return rhi::DebugViewMode::ViewNormal;
    }
    if (name == "metallic_roughness") {
        return rhi::DebugViewMode::MetallicRoughness;
    }
    if (name == "main_depth") {
        return rhi::DebugViewMode::MainDepth;
    }
    if (name == "shadow_depth") {
        return rhi::DebugViewMode::ShadowDepth;
    }
    return std::nullopt;
}

std::expected<void, core::Error> validateObservationSettings(const ObservationSettings& settings) {
    if (settings.version != kObservationSettingsVersion) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "unsupported observation preset version " + std::to_string(settings.version)));
    }
    if (settings.name.find('\0') != std::string::npos || settings.name.size() > 256u) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "observation preset name is invalid"));
    }
    if (!validRelativeSceneRef(settings.sceneRef)) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "observation preset scene_ref must be a clean relative path"));
    }
    const auto& cam = settings.camera;
    if (!allFinite(cam.position) || !allFinite(cam.orientation)) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "observation preset camera position or orientation is non-finite"));
    }
    double quatLenSq = 0.0;
    for (const float component : cam.orientation) {
        quatLenSq += static_cast<double>(component) * component;
    }
    if (!std::isfinite(quatLenSq) || std::abs(quatLenSq - 1.0) > 0.002) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "observation preset camera orientation must be a unit quaternion"));
    }
    if (!std::isfinite(cam.verticalFovRadians) || !(cam.verticalFovRadians > 0.F) ||
        !(cam.verticalFovRadians < std::numbers::pi_v<float>) || !std::isfinite(cam.nearPlane) ||
        !std::isfinite(cam.farPlane) || !(cam.nearPlane > 0.F) || !(cam.farPlane > cam.nearPlane)) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "observation preset camera fov_y or clip planes are out of range"));
    }
    if (!std::isfinite(settings.exposureEV) || std::abs(settings.exposureEV) > kMaxExposureAbsEV) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "observation preset exposure_ev must be finite and within [-16, 16]"));
    }
    if (auto validLight = render::validateDirectionalLight(settings.directionalLight);
        !validLight) {
        return validLight;
    }
    if (static_cast<std::uint32_t>(settings.viewMode) >= rhi::kDebugViewModeCount) {
        return std::unexpected(makeError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            "observation preset view_mode is out of range"));
    }
    return {};
}

std::expected<void, core::Error>
saveObservationSettings(const fs::path& destination, const ObservationSettings& settings) {
    if (auto valid = validateObservationSettings(settings); !valid) {
        return valid;
    }
    Json document{
        {"version", settings.version},
        {"name", settings.name},
        {"scene_ref", settings.sceneRef},
        {"camera",
         {{"position", settings.camera.position},
          {"orientation", settings.camera.orientation},
          {"fov_y", settings.camera.verticalFovRadians},
          {"near", settings.camera.nearPlane},
          {"far", settings.camera.farPlane}}},
        {"exposure_ev", settings.exposureEV},
        {"directional_light",
         {{"direction", settings.directionalLight.direction},
          {"color", settings.directionalLight.color},
          {"intensity", settings.directionalLight.intensity},
          {"cast_shadows", settings.directionalLight.castShadows}}},
        {"view_mode", std::string(debugViewModeName(settings.viewMode))},
    };
    const std::string bytes = document.dump(2) + '\n';
    if (bytes.size() > kMaxPresetBytes) {
        return std::unexpected(fileError(
            core::ErrorCategory::OutputFailed,
            core::ErrorStage::Output,
            destination,
            "serialized observation preset exceeds byte limit"));
    }
    return atomicWriteFile(destination, bytes);
}

std::expected<ObservationSettings, core::Error>
loadObservationSettings(const fs::path& source, std::optional<std::string_view> expectedSceneRef) {
    std::error_code fsError;
    if (source.empty() || !fs::is_regular_file(source, fsError) || fsError) {
        return std::unexpected(fileError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            source,
            "observation preset is missing or not a regular file"));
    }
    std::ifstream stream(source, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > kMaxPresetBytes) {
        return std::unexpected(fileError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            source,
            "could not read observation preset size or file exceeds byte limit"));
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(fileError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            source,
            "could not read observation preset bytes"));
    }
    const Json document = Json::parse(bytes, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return std::unexpected(fileError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            source,
            "invalid observation preset JSON"));
    }

    try {
        const auto& versionValue = document.at("version");
        if (!versionValue.is_number_unsigned()) {
            return std::unexpected(fileError(
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Validation,
                source,
                "version must be an unsigned integer"));
        }
        ObservationSettings settings{};
        settings.version = versionValue.get<std::uint32_t>();
        if (settings.version != kObservationSettingsVersion) {
            return std::unexpected(fileError(
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Validation,
                source,
                "unsupported observation preset version " + std::to_string(settings.version)));
        }
        if (const auto it = document.find("name"); it != document.end()) {
            if (!it->is_string()) {
                throw std::invalid_argument("name must be a string");
            }
            settings.name = it->get<std::string>();
        }
        const auto& sceneRefVal = document.at("scene_ref");
        if (!sceneRefVal.is_string()) {
            throw std::invalid_argument("scene_ref must be a string");
        }
        settings.sceneRef = sceneRefVal.get<std::string>();

        const auto& camVal = document.at("camera");
        if (!camVal.is_object()) {
            throw std::invalid_argument("camera must be an object");
        }
        settings.camera.position = readVector<3>(camVal.at("position"), "camera.position");
        settings.camera.orientation = readVector<4>(camVal.at("orientation"), "camera.orientation");
        settings.camera.verticalFovRadians = readNumber(camVal.at("fov_y"), "camera.fov_y");
        settings.camera.nearPlane = readNumber(camVal.at("near"), "camera.near");
        settings.camera.farPlane = readNumber(camVal.at("far"), "camera.far");

        settings.exposureEV = readNumber(document.at("exposure_ev"), "exposure_ev");

        if (const auto lightIt = document.find("directional_light"); lightIt != document.end()) {
            if (!lightIt->is_object()) {
                throw std::invalid_argument("directional_light must be an object");
            }
            settings.directionalLight.direction =
                readVector<3>(lightIt->at("direction"), "directional_light.direction");
            if (const auto colorIt = lightIt->find("color"); colorIt != lightIt->end()) {
                settings.directionalLight.color =
                    readVector<3>(*colorIt, "directional_light.color");
            }
            if (const auto intIt = lightIt->find("intensity"); intIt != lightIt->end()) {
                settings.directionalLight.intensity =
                    readNumber(*intIt, "directional_light.intensity");
            }
            if (const auto shIt = lightIt->find("cast_shadows"); shIt != lightIt->end()) {
                if (!shIt->is_boolean()) {
                    throw std::invalid_argument("directional_light.cast_shadows must be a boolean");
                }
                settings.directionalLight.castShadows = shIt->get<bool>();
            }
        } else {
            settings.directionalLight = render::defaultDirectionalLight();
            if (const auto shadowIt = document.find("shadows_enabled");
                shadowIt != document.end()) {
                if (!shadowIt->is_boolean()) {
                    throw std::invalid_argument("shadows_enabled must be a boolean");
                }
                settings.directionalLight.castShadows = shadowIt->get<bool>();
            }
        }

        const auto& viewVal = document.at("view_mode");
        if (!viewVal.is_string()) {
            throw std::invalid_argument("view_mode must be a string");
        }
        const auto parsedView = parseDebugViewMode(viewVal.get<std::string>());
        if (!parsedView) {
            throw std::invalid_argument("unknown view_mode: " + viewVal.get<std::string>());
        }
        settings.viewMode = *parsedView;

        if (auto valid = validateObservationSettings(settings); !valid) {
            return std::unexpected(fileError(
                valid.error().category, valid.error().stage, source, valid.error().message));
        }
        if (expectedSceneRef && settings.sceneRef != *expectedSceneRef) {
            return std::unexpected(fileError(
                core::ErrorCategory::InvalidArguments,
                core::ErrorStage::Validation,
                source,
                "preset scene_ref '" + settings.sceneRef + "' does not match active scene '" +
                    std::string(*expectedSceneRef) + "'"));
        }
        return settings;
    } catch (const Json::exception& exception) {
        return std::unexpected(fileError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            source,
            exception.what()));
    } catch (const std::invalid_argument& exception) {
        return std::unexpected(fileError(
            core::ErrorCategory::InvalidArguments,
            core::ErrorStage::Validation,
            source,
            exception.what()));
    }
}

} // namespace ar::runtime
