#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string_view>

#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace {

bool matches(const unsigned char* pixel, const std::array<double, 3>& color, bool srgb) {
    for (std::size_t channel = 0; channel < color.size(); ++channel) {
        const double linear = color[channel];
        const double encoded = srgb ? 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055 : linear;
        if (std::abs(pixel[channel] - std::round(encoded * 255.0)) > 3.0) {
            return false;
        }
    }
    return pixel[3] == 255;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && (argc != 3 || std::wstring_view{argv[2]} != L"set-binding")) {
        return 2;
    }
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, argv[1], L"rb") != 0) {
        return 1;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    const std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(
        stbi_load_from_file(file, &width, &height, &channels, 4), stbi_image_free);
    std::fclose(file);
    if (!pixels || width < 32 || height < 32 || channels != 4) {
        std::fputs("could not decode RGBA screenshot\n", stderr);
        return 1;
    }
    const auto at = [&](int x, int y) {
        return pixels.get() + (static_cast<std::size_t>(y) * width + x) * 4;
    };
    if (argc == 3) {
        // Each vertical region varies only the texture or sampler index relative to region 1.
        constexpr std::array<std::array<unsigned char, 4>, 4> expected{{
            {255, 255, 255, 255}, // Default texture and sampler
            {255, 0, 0, 255},     // Red texel, nearest
            {0, 0, 255, 255},     // Blue texture, nearest
            {128, 128, 0, 255},   // Red/green midpoint, linear (UNORM attachment)
        }};
        constexpr std::array<unsigned, 4> textureIndices{0, 1, 2, 1};
        constexpr std::array<unsigned, 4> samplerIndices{0, 0, 0, 1};
        for (std::size_t region = 0; region < expected.size(); ++region) {
            const int x = static_cast<int>(2 * region + 1) * width / 8;
            const int y = height / 2;
            const auto* pixel = at(x, y);
            auto target = expected[region];
            // The linear midpoint is encoded to approximately 188 on an sRGB swapchain.
            if (region == 3 && pixel[0] > 160) {
                target[0] = target[1] = 188;
            }
            for (std::size_t channel = 0; channel < target.size(); ++channel) {
                if (std::abs(static_cast<int>(pixel[channel]) - target[channel]) > 3) {
                    std::fprintf(
                        stderr,
                        "binding material=%zu texture_index=%u sampler_index=%u pixel=(%d,%d) "
                        "expected=(%u,%u,%u,%u) "
                        "actual=(%u,%u,%u,%u)\n",
                        region,
                        textureIndices[region],
                        samplerIndices[region],
                        x,
                        y,
                        target[0],
                        target[1],
                        target[2],
                        target[3],
                        pixel[0],
                        pixel[1],
                        pixel[2],
                        pixel[3]);
                    return 1;
                }
            }
        }
        return 0;
    }
    constexpr std::array clear{0.025, 0.06, 0.14};
    const bool srgb = matches(at(0, 0), clear, true);
    const std::array corners{
        at(0, 0), at(width - 1, 0), at(0, height - 1), at(width - 1, height - 1)};
    // At NDC (0, 0), the top vertex has weight 0.58 / 1.20.
    constexpr std::array center{0.535, 0.3325, 0.3325};
    if (!std::ranges::all_of(
            corners, [&](const auto* pixel) { return matches(pixel, clear, srgb); }) ||
        !matches(at(width / 2, height / 2), center, srgb)) {
        std::fputs("screenshot center or corners differ from the expected colors\n", stderr);
        return 1;
    }
    std::printf("screenshot_pixels=verified size=%dx%d\n", width, height);
    return 0;
}
