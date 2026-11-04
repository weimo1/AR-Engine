# Fails the configure step unless the toolchain really enables C++23.
#
# A successful compile is not enough: MSVC drops an unsupported /std: switch with warning
# D9002 and keeps the default language mode, so the probe asserts _MSVC_LANG and the
# standard library feature-test macro, and the captured output is scanned for D9002.

function(ar_require_cxx23_preview)
    set(probe_root "${CMAKE_BINARY_DIR}/ar-cxx23-gate")
    file(MAKE_DIRECTORY "${probe_root}")
    file(WRITE "${probe_root}/language_mode_probe.cpp" [=[
#include <expected>
#include <version>

#if !defined(_MSVC_LANG) || (_MSVC_LANG < 202302L)
#error "AR: /std:c++23preview did not enable the C++23 language mode"
#endif

#if !defined(__cpp_lib_expected) || (__cpp_lib_expected < 202202L)
#error "AR: std::expected is not available"
#endif

int main()
{
    return (_MSVC_LANG >= 202302L) ? 0 : 1;
}
]=])

    # try_compile ignores -DCMAKE_CXX_FLAGS passed through CMAKE_FLAGS (verified on CMake 4.2),
    # but it does inherit CMAKE_CXX_FLAGS from this scope. Setting it here keeps the switch out
    # of the real targets, which get it from the ar_cxx23 interface target instead.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++23preview")

    try_compile(
        ar_language_mode_ok
        "${probe_root}/build"
        "${probe_root}/language_mode_probe.cpp"
        OUTPUT_VARIABLE ar_language_mode_output)

    if(ar_language_mode_output MATCHES "D9002")
        message(FATAL_ERROR
            "The compiler ignored an option (warning D9002). P0 requires a compiler that "
            "implements /std:c++23preview; refusing a silently degraded language mode.\n"
            "${ar_language_mode_output}")
    endif()

    if(NOT ar_language_mode_ok)
        message(FATAL_ERROR
            "The C++23 language-mode probe did not compile. P0 requires an effective "
            "/std:c++23preview build (_MSVC_LANG >= 202302 and std::expected).\n"
            "${ar_language_mode_output}")
    endif()

    message(STATUS "ar: C++23 language mode verified (${CMAKE_CXX_COMPILER_VERSION})")
endfunction()
