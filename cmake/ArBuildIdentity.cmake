# Generates the compiled build identity for one configuration.
#
# The single input is AR_BUILD_IDENTITY_FILE: the sidecar written by
# scripts/p0_build_identity.py prepare. CMake never invents identity values; anything
# missing or inconsistent stops the configure step.

function(_ar_escape_cpp_string out value)
    set(text "${value}")
    string(REPLACE "\\" "\\\\" text "${text}")
    string(REPLACE "\"" "\\\"" text "${text}")
    string(REPLACE "\n" "\\n" text "${text}")
    string(REPLACE "\r" "\\r" text "${text}")
    set(${out} "\"${text}\"" PARENT_SCOPE)
endfunction()

function(_ar_read_identity_json raw_json key out_var)
    string(JSON value ERROR_VARIABLE json_error GET "${raw_json}" ${key})
    if(json_error)
        message(FATAL_ERROR "build identity sidecar is missing '${key}': ${json_error}")
    endif()
    if(value STREQUAL "NOTFOUND")
        message(FATAL_ERROR "build identity sidecar has a null '${key}'")
    endif()
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

function(ar_add_build_info target)
    if(NOT DEFINED AR_BUILD_IDENTITY_FILE OR AR_BUILD_IDENTITY_FILE STREQUAL "")
        message(FATAL_ERROR
            "AR_BUILD_IDENTITY_FILE is not set. Configure with "
            "-DAR_BUILD_IDENTITY_FILE=<output>/identity/<configuration>/build-identity.json")
    endif()

    get_filename_component(identity_path "${AR_BUILD_IDENTITY_FILE}" ABSOLUTE)
    if(NOT EXISTS "${identity_path}")
        message(FATAL_ERROR "build identity sidecar does not exist: ${identity_path}")
    endif()

    # A regenerated sidecar must force a reconfigure, otherwise the compiled BuildInfo keeps the
    # previous build_id and the identity test fails against a stale binary.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${identity_path}")

    file(READ "${identity_path}" raw)
    _ar_read_identity_json("${raw}" "schema_version" schema_version)
    if(NOT schema_version STREQUAL "1")
        message(FATAL_ERROR "unsupported build identity schema_version: ${schema_version}")
    endif()

    _ar_read_identity_json("${raw}" "build_id" build_id)
    _ar_read_identity_json("${raw}" "input_hash" input_hash)
    _ar_read_identity_json("${raw}" "configuration" configuration)
    _ar_read_identity_json("${raw}" "toolchain" toolchain)
    _ar_read_identity_json("${raw}" "dependency_manifest_hash" dependency_manifest_hash)
    if(build_id STREQUAL "" OR input_hash STREQUAL "")
        message(FATAL_ERROR "build identity sidecar has empty build_id or input_hash")
    endif()

    # The sidecar must describe the configuration this build directory produces.
    if(DEFINED AR_EXPECTED_CONFIGURATION AND NOT configuration STREQUAL AR_EXPECTED_CONFIGURATION)
        message(FATAL_ERROR
            "build identity sidecar is for configuration '${configuration}' but this build "
            "expects '${AR_EXPECTED_CONFIGURATION}'")
    endif()

    # source.revision is a string, or JSON null when the snapshot carries no VCS metadata, and
    # source.dirty is a JSON boolean, or the string "unknown" for the same reason.
    string(JSON revision_type ERROR_VARIABLE revision_type_error TYPE "${raw}" source revision)
    if(revision_type_error)
        message(FATAL_ERROR "build identity sidecar is missing source.revision: ${revision_type_error}")
    endif()
    if(revision_type STREQUAL "STRING")
        string(JSON source_revision GET "${raw}" source revision)
        set(has_revision TRUE)
        _ar_escape_cpp_string(cpp_revision "${source_revision}")
    elseif(revision_type STREQUAL "NULL")
        set(has_revision FALSE)
        set(cpp_revision "")
    else()
        message(FATAL_ERROR "build identity source.revision must be a string or null")
    endif()

    string(JSON dirty_type ERROR_VARIABLE dirty_type_error TYPE "${raw}" source dirty)
    if(dirty_type_error)
        message(FATAL_ERROR "build identity sidecar is missing source.dirty: ${dirty_type_error}")
    endif()
    if(dirty_type STREQUAL "BOOLEAN")
        # string(JSON ... GET) yields ON/OFF for JSON booleans.
        string(JSON source_dirty GET "${raw}" source dirty)
        if(source_dirty)
            set(cpp_dirty "std::optional<bool>(true)")
        else()
            set(cpp_dirty "std::optional<bool>(false)")
        endif()
    elseif(dirty_type STREQUAL "STRING")
        string(JSON source_dirty GET "${raw}" source dirty)
        if(NOT source_dirty STREQUAL "unknown")
            message(FATAL_ERROR "build identity source.dirty must be a boolean or \"unknown\"")
        endif()
        set(cpp_dirty "std::nullopt")
    else()
        message(FATAL_ERROR "build identity source.dirty must be a boolean or \"unknown\"")
    endif()
    string(JSON triplet ERROR_VARIABLE triplet_error GET "${raw}" toolchain triplet)
    if(triplet_error OR triplet STREQUAL "NOTFOUND")
        message(FATAL_ERROR "build identity sidecar is missing toolchain.triplet")
    endif()

    _ar_escape_cpp_string(cpp_build_id "${build_id}")
    _ar_escape_cpp_string(cpp_input_hash "${input_hash}")
    _ar_escape_cpp_string(cpp_configuration "${configuration}")
    _ar_escape_cpp_string(cpp_triplet "${triplet}")

    if(has_revision)
        set(revision_init "std::optional<std::string>(${cpp_revision})")
    else()
        set(revision_init "std::nullopt")
    endif()

    set(generated_dir "${CMAKE_BINARY_DIR}/generated/ar_build_info")
    file(MAKE_DIRECTORY "${generated_dir}")
    set(generated_source "${generated_dir}/BuildInfo.cpp")

    file(WRITE "${generated_source}"
"// Generated by cmake/ArBuildIdentity.cmake. Do not edit.
#include \"core/BuildInfo.h\"

#include <optional>
#include <string>

#include \"BuildInfoDetail.h\"

namespace ar::core {
namespace {

const char* const kBuildId = ${cpp_build_id};
const char* const kInputHash = ${cpp_input_hash};
const char* const kConfiguration = ${cpp_configuration};
const char* const kTriplet = ${cpp_triplet};

} // namespace

const BuildInfo& buildInfo()
{
    static const BuildInfo info{
        kBuildId,
        kInputHash,
        kConfiguration,
        SourceStamp{${revision_init}, ${cpp_dirty}},
    };
    return info;
}

namespace detail {

const char* buildCrtName()
{
#if defined(_DEBUG)
    return \"/MDd\";
#else
    return \"/MD\";
#endif
}

const char* buildTriplet()
{
    return kTriplet;
}

} // namespace detail
} // namespace ar::core
")

    target_sources(${target} PRIVATE "${generated_source}")
    set_source_files_properties("${generated_source}" PROPERTIES GENERATED TRUE)
    message(STATUS "ar: compiled build identity ${build_id} (${configuration}) from ${identity_path}")
endfunction()
