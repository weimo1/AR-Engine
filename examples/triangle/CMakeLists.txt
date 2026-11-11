find_program(AR_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
find_program(AR_SPIRV_VAL NAMES spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
set(shader_dir "${CMAKE_BINARY_DIR}/shaders")
foreach(stage IN ITEMS vert frag)
    set(source "${PROJECT_SOURCE_DIR}/shaders/triangle.${stage}")
    set(output "${shader_dir}/triangle.${stage}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_dir}"
        COMMAND "${AR_GLSLC}" --target-env=vulkan1.3 "${source}" -o "${output}.tmp"
        COMMAND "${AR_SPIRV_VAL}" --target-env vulkan1.3 "${output}.tmp"
        COMMAND ${CMAKE_COMMAND} -E rename "${output}.tmp" "${output}"
        DEPENDS "${source}" "${AR_GLSLC}" "${AR_SPIRV_VAL}"
        VERBATIM)
    list(APPEND triangle_shaders "${output}")
endforeach()
add_custom_target(ar_triangle_shaders DEPENDS ${triangle_shaders})
set_target_properties(ar_triangle_shaders PROPERTIES FOLDER "Examples")

add_executable(ar_triangle main.cpp)
find_package(Stb REQUIRED)
target_include_directories(ar_triangle SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
add_dependencies(ar_triangle ar_triangle_shaders)
target_link_libraries(ar_triangle PRIVATE ar_runtime)
target_compile_definitions(ar_triangle PRIVATE
    AR_TRIANGLE_OUTPUT_ROOT=u8"${CMAKE_BINARY_DIR}/sample-output/$<CONFIG>"
    AR_TRIANGLE_SHADER_DIR=u8"${shader_dir}"
)
set_target_properties(ar_triangle PROPERTIES
    FOLDER "Examples"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
