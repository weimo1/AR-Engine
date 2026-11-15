#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

#include "render/StaticRenderInput.h"

namespace ar::runtime {

enum class StaticSceneStage { Read, Parse, Validate, Asset, Save };

struct StaticSceneError {
    StaticSceneStage stage;
    std::filesystem::path source;
    std::string message;
};

class StaticSceneSession {
public:
    explicit StaticSceneSession(std::filesystem::path assetRoot);
    ~StaticSceneSession();

    std::expected<void, StaticSceneError> open(const std::filesystem::path& source);
    std::expected<void, StaticSceneError> save(const std::filesystem::path& destination) const;
    std::shared_ptr<const render::StaticRenderInput> input() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace ar::runtime
