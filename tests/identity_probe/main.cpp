// Prints or verifies the identity compiled into ar_core. Exit codes: 0 ok, 2 mismatch,
// 3 usage error. Verification never rewrites the sidecar; a stale binary or sidecar fails.

#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "BuildInfoDetail.h"
#include "core/BuildInfo.h"

#if !defined(_MSVC_LANG)
#error "AR: _MSVC_LANG is required; the P0 target must be built with MSVC."
#endif

// The language mode is part of the acceptance evidence: /std:c++23 silently stays at C++14 on
// this toolset, so the mode is pinned at compile time and reported by --json.
static_assert(_MSVC_LANG >= 202302L, "AR: the build must use /std:c++23preview (C++23 mode).");

namespace {

using nlohmann::json;
using ar::core::BuildInfo;

std::string languageModeName()
{
#if _MSVC_LANG >= 202302L
    return "c++23preview";
#else
    return "c++" + std::to_string(_MSVC_LANG / 100) + std::to_string(_MSVC_LANG % 100);
#endif
}

json identityJson()
{
    const BuildInfo& info = ar::core::buildInfo();
    json encoded;
    encoded["build_id"] = info.buildId;
    encoded["input_hash"] = info.inputHash;
    encoded["configuration"] = info.configuration;
    encoded["source"] = {{"revision", info.source.revision.has_value()
                                      ? json(*info.source.revision)
                                      : json(nullptr)},
                         {"dirty", info.source.dirty.has_value()
                                       ? json(*info.source.dirty)
                                       : json("unknown")}};
    encoded["crt"] = ar::core::detail::buildCrtName();
    encoded["triplet"] = ar::core::detail::buildTriplet();
    encoded["language_mode"] = languageModeName();
    return encoded;
}

int usage(const char* program)
{
    std::fprintf(
        stderr,
        "usage: %s --json | --verify <build-identity.json> [--toolchain <toolchain.json>]\n",
        program);
    return 3;
}

} // namespace

int main(int argc, char** argv)
{
    bool printJson = false;
    std::string sidecarPath;
    std::string toolchainPath;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--json") {
            printJson = true;
        } else if (argument == "--verify" && index + 1 < argc) {
            sidecarPath = argv[++index];
        } else if (argument == "--toolchain" && index + 1 < argc) {
            toolchainPath = argv[++index];
        } else {
            return usage(argv[0]);
        }
    }

    const json compiled = identityJson();
    if (printJson) {
        std::printf("%s\n", compiled.dump(2).c_str());
        return 0;
    }
    if (sidecarPath.empty()) {
        return usage(argv[0]);
    }

    std::ifstream sidecarStream(sidecarPath, std::ios::binary);
    if (!sidecarStream.is_open()) {
        std::fprintf(stderr, "identity-mismatch: cannot read sidecar %s\n", sidecarPath.c_str());
        return 2;
    }
    json sidecar;
    try {
        sidecarStream >> sidecar;
    } catch (const json::exception& ex) {
        std::fprintf(stderr, "identity-mismatch: sidecar is not valid JSON: %s\n", ex.what());
        return 2;
    }

    int mismatches = 0;
    auto compare = [&mismatches](const char* field, const json& recorded, const json& actual) {
        if (recorded != actual) {
            std::fprintf(
                stderr, "identity-mismatch: %s recorded=%s compiled=%s\n", field,
                recorded.dump().c_str(), actual.dump().c_str());
            ++mismatches;
        }
    };

    compare("build_id", sidecar.value("build_id", json("")), compiled["build_id"]);
    compare("input_hash", sidecar.value("input_hash", json("")), compiled["input_hash"]);
    compare("configuration", sidecar.value("configuration", json("")),
            compiled["configuration"]);
    if (sidecar.contains("source")) {
        compare("source.revision", sidecar["source"].value("revision", json("")),
                compiled["source"]["revision"]);
        compare("source.dirty", sidecar["source"].value("dirty", json("")),
                compiled["source"]["dirty"]);
    } else {
        std::fprintf(stderr, "identity-mismatch: sidecar has no source stamp\n");
        ++mismatches;
    }

    if (!toolchainPath.empty()) {
        std::ifstream toolchainStream(toolchainPath, std::ios::binary);
        if (!toolchainStream.is_open()) {
            std::fprintf(stderr, "identity-mismatch: cannot read %s\n", toolchainPath.c_str());
            return 2;
        }
        json toolchain;
        try {
            toolchainStream >> toolchain;
        } catch (const json::exception& ex) {
            std::fprintf(stderr, "identity-mismatch: toolchain is not valid JSON: %s\n",
                         ex.what());
            return 2;
        }
        compare("toolchain.triplet", toolchain.value("triplet", json("")), compiled["triplet"]);
        const std::string crtKey =
            compiled["configuration"] == "Debug" ? "debug" : "optimized";
        json expectedCrt = json("");
        if (toolchain.contains("crt") && toolchain["crt"].is_object()) {
            expectedCrt = toolchain["crt"].value(crtKey, json(""));
        }
        compare("toolchain.crt", expectedCrt, compiled["crt"]);
    }

    return mismatches == 0 ? 0 : 2;
}
