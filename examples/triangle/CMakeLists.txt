find_program(AR_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
find_program(AR_SPIRV_VAL NAMES spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
set(shader_dir "${CMAKE_BINARY_DIR}/shaders")
set(binding_limits "${PROJECT_SOURCE_DIR}/engine/rhi/src/vulkan/binding/SetBindingLimits.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${binding_limits}")
file(READ "${binding_limits}" binding_constants)
string(REGEX MATCH "kSetImageCount = ([0-9]+)" image_constant "${binding_constants}")
set(binding_images "${CMAKE_MATCH_1}")
string(REGEX MATCH "kSetSamplerCount = ([0-9]+)" sampler_constant "${binding_constants}")
set(binding_samplers "${CMAKE_MATCH_1}")
if(NOT image_constant OR NOT sampler_constant)
    message(FATAL_ERROR "Descriptor set capacities are missing")
endif()

foreach(shader IN ITEMS triangle.vert triangle.frag set_binding.vert set_binding.frag)
    set(source "${PROJECT_SOURCE_DIR}/shaders/${shader}")
    set(output "${shader_dir}/${shader}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_dir}"
        COMMAND "${AR_GLSLC}" --target-env=vulkan1.3 "-DAR_SET_IMAGE_COUNT=${binding_images}" "-DAR_SET_SAMPLER_COUNT=${binding_samplers}" "${source}" -o "${output}.tmp"
        COMMAND "${AR_SPIRV_VAL}" --target-env vulkan1.3 "${output}.tmp"
        COMMAND ${CMAKE_COMMAND} -E rename "${output}.tmp" "${output}"
        DEPENDS "${source}" "${binding_limits}" "${AR_GLSLC}" "${AR_SPIRV_VAL}"
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
