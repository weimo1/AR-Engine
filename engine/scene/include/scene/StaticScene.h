#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ar::scene {

enum class FileStage { Read, Parse, Validate, Save };

struct FileError {
    FileStage stage;
    std::filesystem::path source;
    std::string message;
};

struct Transform {
    std::array<float, 3> position;
    std::array<float, 4> rotation;
    std::array<float, 3> scale;
};

struct Instance {
    std::uint64_t id;
    std::filesystem::path asset;
    Transform transform;
};

struct Camera {
    std::array<float, 3> position;
    std::array<float, 4> orientation;
    float verticalFovRadians;
    float nearPlane;
    float farPlane;
};

class StaticScene {
public:
    static std::expected<StaticScene, FileError> load(const std::filesystem::path& source);
    std::expected<void, FileError> save(const std::filesystem::path& destination) const;

    ~StaticScene();
    StaticScene(StaticScene&&) noexcept;
    StaticScene& operator=(StaticScene&&) noexcept;

    Camera camera() const;
    std::vector<Instance> instances() const;

private:
    struct Data;
    explicit StaticScene(std::unique_ptr<Data> data);
    std::unique_ptr<Data> m_data;
};

} // namespace ar::scene
