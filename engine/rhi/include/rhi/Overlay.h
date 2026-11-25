#pragma once

#include <cstdint>
#include <vector>

namespace ar::rhi {

// Matches Dear ImGui's default 20-byte ImDrawVert layout (pos, uv, packed ABGR8 color).
struct OverlayVertex {
    float x = 0.F;
    float y = 0.F;
    float u = 0.F;
    float v = 0.F;
    std::uint32_t rgba8 = 0u;
};

static_assert(sizeof(OverlayVertex) == 20u);

struct OverlayDrawCommand {
    std::uint32_t indexOffset = 0u;
    std::uint32_t indexCount = 0u;
    std::int32_t vertexOffset = 0;
    std::int32_t clipX = 0;
    std::int32_t clipY = 0;
    std::uint32_t clipWidth = 0u;
    std::uint32_t clipHeight = 0u;
};

struct OverlayDrawList {
    float displayWidth = 0.F;
    float displayHeight = 0.F;
    std::vector<OverlayVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<OverlayDrawCommand> commands;
};

struct OverlayFontAtlas {
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::vector<std::uint8_t> rgba;
};

struct OverlayShaders {
    std::vector<std::uint32_t> vertex;
    std::vector<std::uint32_t> fragment;
};

} // namespace ar::rhi
