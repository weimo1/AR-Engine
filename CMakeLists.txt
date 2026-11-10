cmake_minimum_required(VERSION 4.2)

project(ArEngine VERSION 0.1.0 LANGUAGES CXX)

# Group the generated Visual Studio solution. Every target assigns a FOLDER; see the per-module
# CMakeLists.txt files (Engine) and tests/CMakeLists.txt (Tests, TestSupport).
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT MSVC)
    message(FATAL_ERROR "AR Engine P0 requires MSVC from Visual Studio 2026.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "AR Engine P0 requires an x64 target.")
endif()

foreach(flag_variable CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
    if("${${flag_variable}}" MATCHES "std:c\\+\\+latest")
        message(FATAL_ERROR
            "${flag_variable} selects /std:c++latest. P0 requires /std:c++23preview only.")
    endif()
endforeach()

# The preview switch is the only supported C++23 mode for MSVC 14.51: /std:c++23 compiles but
# silently keeps _MSVC_LANG at C++14, which the language-mode gate catches.
#
# The Visual Studio generator does not copy this option into AdditionalOptions; it maps it to
# <LanguageStandard>stdcpp23</LanguageStandard>. MSVC 14.51 renders that property value as
# /std:c++23preview (verified against the captured CL.exe command lines), so targets link
# ar_cxx23 and exactly one /std: switch reaches every compile.
add_library(ar_cxx23 INTERFACE)
target_compile_options(ar_cxx23 INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:/std:c++23preview>")

# Exceptions are part of the error contract: core catches spdlog failures and tests rely on
# Catch2's REQUIRE machinery. Without /EHsc every translation unit warns C4530 and the unwind
# behaviour of catch blocks is not what the code assumes.
add_library(ar_cxx_flags INTERFACE)
target_compile_options(ar_cxx_flags INTERFACE
    "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
    "$<$<COMPILE_LANGUAGE:CXX>:/W4>"
    "$<$<COMPILE_LANGUAGE:CXX>:/permissive->"
    "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>"
)

# Every target links the shared CRT: /MD for Release-like configurations, /MDd for Debug.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(VCPKG_TARGET_TRIPLET AND NOT VCPKG_TARGET_TRIPLET STREQUAL "x64-windows-static-md")
    message(FATAL_ERROR
        "P0 requires VCPKG_TARGET_TRIPLET=x64-windows-static-md, got '${VCPKG_TARGET_TRIPLET}'.")
endif()

# Machine-specific locations come from the ignored CMakeUserPresets.json (VCPKG_ROOT, AR_P0_DEPS)
# or from an explicit command line; failing here keeps a half-configured tree from looking usable.
if(NOT CMAKE_TOOLCHAIN_FILE OR NOT VCPKG_INSTALLED_DIR)
    message(FATAL_ERROR
        "Configure through the win-vs2026-* presets with a local CMakeUserPresets.json that sets "
        "VCPKG_ROOT and AR_P0_DEPS, or pass CMAKE_TOOLCHAIN_FILE and VCPKG_INSTALLED_DIR explicitly.")
endif()

if(NOT AR_BUILD_IDENTITY_FILE)
    message(FATAL_ERROR
        "AR_BUILD_IDENTITY_FILE is required. Run scripts/p0_build_identity.py prepare first.")
endif()

if(NOT AR_EXPECTED_CONFIGURATION)
    message(FATAL_ERROR
        "AR_EXPECTED_CONFIGURATION is required so a sidecar generated for another configuration "
        "is rejected instead of compiled in.")
endif()

set(AR_P0_REQUIRED_CONFIGURATIONS Debug RelWithDebInfo)

# The diagnostics fault-injection hooks exist only for tests. Production builds keep them out of
# ar_core so a shipped static library has no test entry points.
option(AR_ENABLE_TEST_HOOKS "Compile the test-only diagnostics fault-injection hooks" OFF)

# The suite is on by default so the verify command table always registers every required case;
# integration consumers may build the engine without a test suite.
option(AR_BUILD_TESTS "Build and register the CTest suite" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ArCompilerGate)
include(ArBuildIdentity)

ar_require_cxx23_preview()

add_subdirectory(engine/core)
add_subdirectory(engine/rhi)
add_subdirectory(engine/runtime)

if(AR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
