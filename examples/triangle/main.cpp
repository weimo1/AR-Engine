#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/RuntimeSession.h"

namespace {

struct Options {
    std::optional<std::uint32_t> frames;
    bool visible = true;
    bool clearOnly = false;
    std::filesystem::path output = AR_TRIANGLE_OUTPUT_ROOT;
};

std::optional<Options> parse(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view arg = argv[index];
        if (arg == L"--hidden") {
            options.visible = false;
        } else if (arg == L"--clear-only") {
            options.clearOnly = true;
        } else if (arg == L"--frames" && !options.frames && index + 1 < argc) {
            const std::wstring_view value = argv[++index];
            if (value.empty() || value.find_first_not_of(L"0123456789") != value.npos) {
                return std::nullopt;
            }
            errno = 0;
            const unsigned long frames = std::wcstoul(value.data(), nullptr, 10);
            if (errno == ERANGE || frames == 0) {
                return std::nullopt;
            }
            options.frames = static_cast<std::uint32_t>(frames);
        } else if (arg == L"--output" && index + 1 < argc) {
            options.output = argv[++index];
            if (options.output.empty()) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }
    if (!options.visible && !options.frames) {
        return std::nullopt;
    }
    return options;
}

std::optional<std::vector<std::uint32_t>> readShader(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto bytes = static_cast<std::streamoff>(input.tellg());
    constexpr auto wordSize = static_cast<std::streamoff>(sizeof(std::uint32_t));
    constexpr std::streamoff maxShaderBytes = 1 << 20;
    if (bytes < 5 * wordSize || bytes > maxShaderBytes || bytes % wordSize != 0) {
        std::fwprintf(stderr, L"Missing or invalid shader: %ls\n", path.c_str());
        return std::nullopt;
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes / wordSize));
    if (!input.seekg(0).read(reinterpret_cast<char*>(words.data()), bytes)) {
        std::fwprintf(stderr, L"Could not read shader: %ls\n", path.c_str());
        return std::nullopt;
    }
    return words;
}

void printError(const ar::core::Error& error) {
    std::fprintf(stderr, "%s: %s", ar::core::toString(error.category), error.message.c_str());
    if (error.origin) {
        std::fprintf(
            stderr,
            " (%s %lld)",
            error.origin->domain.c_str(),
            static_cast<long long>(error.origin->code));
    }
    std::fputc('\n', stderr);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto options = parse(argc, argv);
    if (!options) {
        std::fputs(
            "usage: ar_triangle [--frames N] [--hidden] [--clear-only] [--output DIRECTORY]\n",
            stderr);
        return 2;
    }

    ar::runtime::RuntimeConfig config;
    config.mode = ar::runtime::QueryMode::Development;
    config.query.requestedApi = ar::rhi::makeApiVersion(1, 3);
    config.query.createGraphicsDevice = true;
    if (!options->clearOnly) {
        const std::filesystem::path shaderDir = AR_TRIANGLE_SHADER_DIR;
        auto vertex = readShader(shaderDir / "triangle.vert.spv");
        auto fragment = readShader(shaderDir / "triangle.frag.spv");
        if (!vertex || !fragment) {
            return 1;
        }
        config.query.triangleShaders =
            ar::rhi::TriangleShaders{std::move(*vertex), std::move(*fragment)};
    }
    config.window.emplace();
    config.window->title = "AR Engine Triangle";
    config.window->visible = options->visible;
    config.diagnostics.outputRoot = options->output;
    const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    config.diagnostics.runId = "triangle-" + std::to_string(timestamp);

    auto session = ar::runtime::RuntimeSession::start(config);
    if (!session) {
        const auto& error = session.error().endResult.primaryError;
        if (error) {
            printError(*error);
        }
        return 1;
    }

    unsigned long long presented = 0;
    while (!options->frames || presented < *options->frames) {
        const auto frame = (*session)->presentFrame();
        if (!frame || !*frame) {
            break;
        }
        ++presented;
    }
    const ar::core::EndResult result = (*session)->close();
    std::printf("presented_frames=%llu\n", presented);
    if (result.primaryError) {
        printError(*result.primaryError);
    }
    for (const auto& error : result.cleanupErrors) {
        printError(error);
    }
    for (const auto& error : result.diagnosticErrors) {
        printError(error);
    }
    if (result.result != ar::core::ResultKind::Success) {
        return 1;
    }
    if (options->frames && presented != *options->frames) {
        std::fputs("window closed before the requested frame count\n", stderr);
        return 1;
    }
    return 0;
}
