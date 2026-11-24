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
    core-capability-combinations
    core-summary-conflicts
    core-event-encoding-failure
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
    rhi-device-create
    rhi-device-vulkan14-heap
    rhi-vma-buffer
    rhi-vma-image-view
    rhi-upload-readback
    rhi-resource-retirement
    rhi-upload-retirement
    rhi-set-binding-limits
    rhi-descriptor-heap-layout
    rhi-descriptor-heap-readback
    rhi-descriptor-heap-retirement
    rhi-set-binding-retirement
    rhi-gpu-culling-reference
    rhi-set-binding-device
    rhi-device-enhanced-unavailable
    rhi-device-missing-feature
    rhi-device-selection
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

set_tests_properties(rhi-device-create rhi-device-vulkan14-heap rhi-vma-buffer rhi-vma-image-view
    PROPERTIES LABELS gpu TIMEOUT 30)
set_tests_properties(rhi-upload-readback rhi-upload-retirement rhi-set-binding-device rhi-gpu-culling-reference rhi-descriptor-heap-readback rhi-descriptor-heap-retirement PROPERTIES LABELS gpu TIMEOUT 30
    ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>;VK_VALIDATION_VALIDATE_SYNC=1")

set(AR_REQUIRED_RUNTIME_CASES
    runtime-rollback
    runtime-device-none
    runtime-device-index-invalid
    runtime-cleanup-failure
    runtime-cleanup-empty
    runtime-quarantine-reopen
    runtime-repeat-close
    runtime-summary-capability
    runtime-system-inspect
    runtime-system-development
    runtime-require-product
    runtime-mode-mismatch
    runtime-diagnostic-create-failure
    runtime-summary-output-failure
    runtime-capability-encode-failure
    runtime-destructor
    rhi-descriptor-heap-scene-layout
    sample-resize
    sample-camera-view-update
    sample-static-scene-heap-gbuffer
    sample-static-scene-heap-gpu-driven
    sample-directional-shadow-parity
    sample-free-camera-shadows
)

add_executable(ar_runtime_tests
    runtime/test_runtime_session.cpp
)
target_link_libraries(ar_runtime_tests
    PRIVATE
        ar_runtime
        Catch2::Catch2WithMain
        nlohmann_json::nlohmann_json
)
# The runtime cases read the rhi process counters and quarantine flag, which are private hooks.
target_include_directories(ar_runtime_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/engine/rhi/src"
)
target_compile_definitions(ar_runtime_tests PRIVATE
    AR_ENABLE_TEST_HOOKS=1
    AR_TEST_SHADER_DIR=u8"${CMAKE_BINARY_DIR}/shaders"
    AR_SHOWROOM_DEPLOYED_DIR=u8"$<TARGET_FILE_DIR:ar_triangle>/content/showroom"
    AR_TRIANGLE_EXECUTABLE=u8"$<TARGET_FILE:ar_triangle>"
)
add_dependencies(ar_runtime_tests ar_rhi_fixture ar_triangle_shaders ar_triangle)
foreach(case IN LISTS AR_REQUIRED_RUNTIME_CASES)
    add_test(NAME ${case} COMMAND ar_runtime_tests "[${case}]")
    set_tests_properties(${case} PROPERTIES
        ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>"
    )
endforeach()
# These two open the machine's real Vulkan loader. The label lets a machine without one (the
# hosted CI runner) leave them out with `ctest -LE gpu`; keep it in step with gpu_cases in
# workflow/p0-checks.json.
set_tests_properties(runtime-system-inspect runtime-system-development PROPERTIES LABELS gpu)
set_tests_properties(sample-resize sample-camera-view-update sample-static-scene-heap-gbuffer sample-static-scene-heap-gpu-driven sample-directional-shadow-parity sample-free-camera-shadows PROPERTIES LABELS gpu TIMEOUT 90
    ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>;VK_VALIDATION_VALIDATE_SYNC=1")

add_executable(ar_platform_tests platform/test_window.cpp platform/test_window_input.cpp)
target_link_libraries(ar_platform_tests PRIVATE ar_platform Catch2::Catch2WithMain)
foreach(case IN ITEMS platform-window-create platform-window-vulkan-extensions platform-input-state platform-window-input)
    add_test(NAME ${case} COMMAND ar_platform_tests "[${case}]")
    set_tests_properties(${case} PROPERTIES TIMEOUT 15)
endforeach()
set_tests_properties(platform-window-vulkan-extensions PROPERTIES LABELS gpu)

add_test(NAME sample-clear-frames
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_clear_frames.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>" clear
)
add_test(NAME sample-triangle-frames
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_clear_frames.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>" triangle
)
find_package(Stb REQUIRED)
add_executable(ar_verify_screenshot triangle/verify_screenshot.cpp)
target_include_directories(ar_verify_screenshot SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
target_link_libraries(ar_verify_screenshot PRIVATE ar_cxx23 ar_cxx_flags)
set_target_properties(ar_verify_screenshot PROPERTIES FOLDER "TestSupport")
add_test(NAME sample-triangle-screenshot
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_clear_frames.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>" screenshot
        "$<TARGET_FILE:ar_verify_screenshot>"
)
add_test(NAME sample-set-binding-pixels
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_clear_frames.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>" set-binding
        "$<TARGET_FILE:ar_verify_screenshot>"
)
add_test(NAME sample-set-binding-retirement
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_clear_frames.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>" set-binding-retirement
)
add_test(NAME sample-gpu-driven-pixels
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_clear_frames.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>" gpu-driven
        "$<TARGET_FILE:ar_verify_screenshot>"
)
add_test(NAME shader-build COMMAND ${CMAKE_COMMAND}
    "-DVALIDATOR=${AR_SPIRV_VAL}" "-DSHADER_DIR=${CMAKE_BINARY_DIR}/shaders"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_shaders.cmake")
set_tests_properties(sample-clear-frames sample-triangle-frames sample-triangle-screenshot sample-set-binding-pixels sample-set-binding-retirement sample-gpu-driven-pixels PROPERTIES
    LABELS gpu TIMEOUT 50 ENVIRONMENT "VK_VALIDATION_VALIDATE_SYNC=1"
)

# F5 launches the executable directly, so CTest's ENVIRONMENT property does not apply.
foreach(test_target IN ITEMS ar_core_tests ar_rhi_tests ar_runtime_tests)
    set_target_properties(${test_target} PROPERTIES
        VS_DEBUGGER_ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>"
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
    public_headers/header_rhi_buffer.cpp
    public_headers/header_rhi_image.cpp
    public_headers/header_rhi_presentation_spec.cpp
    public_headers/header_rhi_session.cpp
    public_headers/header_rhi_verification.cpp
    public_headers/header_runtime_config.cpp
    public_headers/header_runtime_session.cpp
    public_headers/header_platform_window.cpp
)
target_link_libraries(ar_public_header_probe PRIVATE ar_core ar_rhi ar_runtime ar_platform)
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

# Visual Studio solution folders. The suites are the entry points a developer runs; the
# fixture loader and the two probes only exist to serve them.
set_target_properties(ar_core_tests ar_rhi_tests ar_runtime_tests ar_platform_tests
    PROPERTIES FOLDER "Tests")
set_target_properties(ar_rhi_fixture ar_identity_probe ar_public_header_probe
    PROPERTIES FOLDER "TestSupport")

add_test(NAME sample-static-scene-pixels
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_static_scene.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>"
        "$<TARGET_FILE:ar_verify_screenshot>")
set_tests_properties(sample-static-scene-pixels PROPERTIES
    LABELS "gpu"
    ENVIRONMENT "VK_VALIDATION_VALIDATE_SYNC=1")

add_test(NAME sample-gbuffer-probes
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_gbuffer.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>")
set_tests_properties(sample-gbuffer-probes PROPERTIES
    LABELS "gpu"
    ENVIRONMENT "VK_VALIDATION_VALIDATE_SYNC=1")

add_test(NAME sample-bindless-material-probes
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_gbuffer.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>"
        --bindless-materials)
set_tests_properties(sample-bindless-material-probes PROPERTIES
    LABELS "gpu"
    ENVIRONMENT "VK_VALIDATION_VALIDATE_SYNC=1")

add_test(NAME sample-static-scene-gpu-driven
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_static_scene_gpu_driven.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>")
set_tests_properties(sample-static-scene-gpu-driven PROPERTIES
    LABELS "gpu"
    ENVIRONMENT "VK_VALIDATION_VALIDATE_SYNC=1")

add_test(NAME sample-hdr-lighting-probes
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_hdr_lighting.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>")
set_tests_properties(sample-hdr-lighting-probes PROPERTIES
    LABELS "gpu"
    ENVIRONMENT "VK_VALIDATION_VALIDATE_SYNC=1")

add_test(NAME sample-ibl-probes
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_ibl.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>")
set_tests_properties(sample-ibl-probes PROPERTIES
    LABELS "gpu"
    ENVIRONMENT "VK_VALIDATION_VALIDATE_SYNC=1")

add_test(NAME sample-descriptor-heap-scene-parity
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_descriptor_heap_scene.py"
        "$<TARGET_FILE:ar_triangle>" "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>" "$<CONFIG>")
set_tests_properties(sample-descriptor-heap-scene-parity PROPERTIES
    LABELS "gpu" TIMEOUT 60
    ENVIRONMENT "VK_VALIDATION_VALIDATE_SYNC=1")

add_executable(ar_render_tests
    render/test_cull_reference.cpp
    render/test_directional_shadow.cpp
    render/test_environment_lighting.cpp
    render/test_frame_view.cpp
    render/test_free_camera.cpp
    render/test_presentation_spec.cpp
    render/test_render_graph.cpp
    render/test_static_scene_draw_data.cpp
)
target_link_libraries(ar_render_tests PRIVATE ar_render ar_assets ar_scene Catch2::Catch2WithMain)
set_target_properties(ar_render_tests PROPERTIES
    FOLDER "Tests"
    VS_DEBUGGER_ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>"
)
add_test(NAME render-culling-reference COMMAND ar_render_tests "[render-culling-reference]")
add_test(NAME render-directional-shadow COMMAND ar_render_tests "[render-directional-shadow]")
set_tests_properties(render-directional-shadow PROPERTIES
    ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>"
)
add_test(NAME render-environment-lighting COMMAND ar_render_tests "[render-environment-lighting]")
add_test(NAME render-frame-view COMMAND ar_render_tests "[render-frame-view]")
add_test(NAME render-free-camera COMMAND ar_render_tests "[render-free-camera]")
add_test(NAME render-presentation-spec COMMAND ar_render_tests "[render-presentation-spec]")
add_test(NAME render-graph-plan COMMAND ar_render_tests "[render-graph-plan]")
add_test(NAME render-static-scene-draw-data COMMAND ar_render_tests "[render-static-scene-draw-data]")

add_executable(ar_assets_tests assets/test_static_asset.cpp)
target_link_libraries(ar_assets_tests PRIVATE ar_assets ar_runtime Catch2::Catch2WithMain)
add_test(NAME assets-static-gltf COMMAND ar_assets_tests "[assets-static-gltf]")
add_test(NAME assets-opaque-material COMMAND ar_assets_tests "[assets-opaque-material]")
set_tests_properties(assets-static-gltf assets-opaque-material PROPERTIES
    ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>")
add_test(NAME scene-static-roundtrip COMMAND ar_assets_tests "[scene-static-roundtrip]")
set_tests_properties(scene-static-roundtrip PROPERTIES
    ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>")

target_link_libraries(ar_rhi_tests PRIVATE ar_render)
add_dependencies(ar_rhi_tests ar_triangle_shaders)
target_compile_definitions(ar_rhi_tests PRIVATE
    AR_GPU_CULL_SHADER=u8"${CMAKE_BINARY_DIR}/shaders/gpu_cull.comp.spv"
    AR_HEAP_PROBE_SHADER=u8"${CMAKE_BINARY_DIR}/shaders/heap_probe.comp.spv")

add_executable(ar_app_tests
    triangle/test_app_paths.cpp
    "${PROJECT_SOURCE_DIR}/examples/triangle/AppPaths.cpp"
)
target_include_directories(ar_app_tests PRIVATE "${PROJECT_SOURCE_DIR}/examples/triangle")
target_link_libraries(ar_app_tests PRIVATE ar_assets ar_render ar_runtime Catch2::Catch2WithMain)
add_dependencies(ar_app_tests ar_triangle_shaders ar_triangle_content)
target_compile_definitions(ar_app_tests PRIVATE
    AR_SHOWROOM_SOURCE_DIR=u8"${PROJECT_SOURCE_DIR}/examples/triangle/content/showroom"
    AR_SHOWROOM_DEPLOYED_DIR=u8"$<TARGET_FILE_DIR:ar_triangle>/content/showroom"
    AR_TRIANGLE_DEPLOYED_SHADER_DIR=u8"$<TARGET_FILE_DIR:ar_triangle>/shaders"
)
set_target_properties(ar_app_tests PROPERTIES
    FOLDER "Tests"
    VS_DEBUGGER_ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>"
)
add_test(NAME app-paths-and-showroom-cli COMMAND ar_app_tests "[app-paths-and-showroom-cli]")
set_tests_properties(app-paths-and-showroom-cli PROPERTIES
    ENVIRONMENT "AR_TEST_OUTPUT_ROOT=${AR_TEST_OUTPUT_ROOT}/$<CONFIG>"
)

add_test(NAME sample-showroom-launch
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/triangle/check_showroom_launch.py"
        "$<TARGET_FILE:ar_triangle>"
        "${AR_TEST_OUTPUT_ROOT}/$<CONFIG>"
        "$<CONFIG>"
        "${PROJECT_SOURCE_DIR}/examples/triangle/content/showroom"
)
set_tests_properties(sample-showroom-launch PROPERTIES
    LABELS "gpu" TIMEOUT 90
    ENVIRONMENT "VK_VALIDATION_VALIDATE_SYNC=1"
)
