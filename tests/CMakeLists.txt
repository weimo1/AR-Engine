find_package(Catch2 3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(NOT DEFINED AR_BUILD_IDENTITY_FILE)
    message(FATAL_ERROR "tests require AR_BUILD_IDENTITY_FILE from the identity sidecar")
endif()
if(NOT AR_ENABLE_TEST_HOOKS)
    message(FATAL_ERROR
        "A test-enabled configure requires -DAR_ENABLE_TEST_HOOKS=ON: without the hooks the "
        "flush-failure case would silently disappear from the required case set. Build a "
        "production tree with -DAR_BUILD_TESTS=OFF instead of shrinking the suite.")
endif()
get_filename_component(_ar_identity_dir "${AR_BUILD_IDENTITY_FILE}" DIRECTORY)
get_filename_component(_ar_identity_root "${_ar_identity_dir}/.." ABSOLUTE)
get_filename_component(AR_P0_OUTPUT_ROOT "${_ar_identity_root}/.." ABSOLUTE)
set(AR_TEST_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/test-output")

add_executable(ar_core_tests
    core/test_diagnostics.cpp
)
target_link_libraries(ar_core_tests
    PRIVATE
        ar_core
        Catch2::Catch2WithMain
        nlohmann_json::nlohmann_json
)
target_compile_options(ar_core_tests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<CXX_COMPILER_ID:MSVC>:/permissive->
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
# The flush fault injection lives in a private core header; it is not part of the public API.
target_include_directories(ar_core_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/engine/core/src"
)
if(AR_ENABLE_TEST_HOOKS)
    target_compile_definitions(ar_core_tests PRIVATE AR_ENABLE_TEST_HOOKS=1)
endif()

set(AR_REQUIRED_CORE_CASES
    core-normal-close
    core-open-failed
    core-json-escaping
    core-quota
    core-quota-cumulative
    core-summary-truncated
    core-repeat-finish
    core-late-event
    report-unwritable
    core-flush-failure
)
foreach(case IN LISTS AR_REQUIRED_CORE_CASES)
    add_test(NAME ${case} COMMAND ar_core_tests "[${case}]")
    set_tests_properties(${case} PROPERTIES
        ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>"
    )
endforeach()

set(AR_REQUIRED_RHI_CASES
    rhi-request-validation
    rhi-loader-unavailable
    rhi-loader-symbol-missing
    rhi-device-none
    rhi-device-index-out-of-range
    rhi-api-version-unsupported
    rhi-api-variant-unsupported
    rhi-instance-incompatible
    rhi-instance-init-failed
    rhi-extension-query-failed
    rhi-device-extension-query-failed
    rhi-enumeration-incomplete
    rhi-device-version-insufficient
    rhi-descriptor-heap-present
    rhi-cleanup-instance-retained
    rhi-repeat-close
    rhi-report-after-close
    rhi-quarantine-reopen
)

# A stub loader, so loader/instance/enumeration/cleanup branches run without a GPU.
find_package(VulkanHeaders CONFIG REQUIRED)
add_library(ar_rhi_fixture SHARED support/rhi_fixture/FixtureLoader.cpp)
target_link_libraries(ar_rhi_fixture PRIVATE Vulkan::Headers ar_cxx23 ar_cxx_flags)
target_compile_options(ar_rhi_fixture PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<CXX_COMPILER_ID:MSVC>:/permissive->
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
# The fixture exports the plain C entry point the loader contract names.
set_target_properties(ar_rhi_fixture PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_executable(ar_rhi_tests
    rhi/test_rhi_session.cpp
)
target_link_libraries(ar_rhi_tests
    PRIVATE
        ar_rhi
        Catch2::Catch2WithMain
)
target_include_directories(ar_rhi_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/engine/rhi/src"
)
target_compile_definitions(ar_rhi_tests PRIVATE AR_ENABLE_TEST_HOOKS=1)
add_dependencies(ar_rhi_tests ar_rhi_fixture)
foreach(case IN LISTS AR_REQUIRED_RHI_CASES)
    add_test(NAME ${case} COMMAND ar_rhi_tests "[${case}]")
    set_tests_properties(${case} PROPERTIES
        ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>"
    )
endforeach()

# Each translation unit below includes exactly one public header, so a successful compile is
# the self-containment proof. The test only asserts the objects for this configuration exist.
add_library(ar_public_header_probe OBJECT
    public_headers/header_error.cpp
    public_headers/header_build_info.cpp
    public_headers/header_diagnostics.cpp
    public_headers/header_rhi_loader_source.cpp
    public_headers/header_rhi_capabilities.cpp
    public_headers/header_rhi_session.cpp
)
target_link_libraries(ar_public_header_probe PRIVATE ar_core ar_rhi)
add_test(NAME public-headers
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/check_public_headers.py"
        --search-root "${CMAKE_CURRENT_BINARY_DIR}"
        --configuration "$<CONFIG>"
)

add_executable(ar_identity_probe
    identity_probe/main.cpp
)
target_link_libraries(ar_identity_probe
    PRIVATE
        ar_core
        nlohmann_json::nlohmann_json
)
target_include_directories(ar_identity_probe
    PRIVATE "${PROJECT_SOURCE_DIR}/engine/core/src"
)

add_test(NAME build-identity
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/check_build_identity.py"
        --probe "$<TARGET_FILE:ar_identity_probe>"
        --python "${Python3_EXECUTABLE}"
        --identity-script "${PROJECT_SOURCE_DIR}/scripts/p0_build_identity.py"
        --snapshot "${PROJECT_SOURCE_DIR}"
        --output-root "${AR_P0_OUTPUT_ROOT}"
        --metadata-dir "${AR_P0_OUTPUT_ROOT}/metadata"
        --configuration "$<CONFIG>"
        --test-hooks "$<IF:$<BOOL:${AR_ENABLE_TEST_HOOKS}>,on,off>"
)
