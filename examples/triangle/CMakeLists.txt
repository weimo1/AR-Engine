find_program(AR_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
find_program(AR_SPIRV_VAL NAMES spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(shader_dir "${CMAKE_BINARY_DIR}/shaders")
set(triangle_runtime_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
set(triangle_runtime_shader_dir "${triangle_runtime_dir}/shaders")
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

foreach(shader IN ITEMS triangle.vert triangle.frag set_binding.vert set_binding.frag gpu_cull.comp heap_probe.comp gpu_draw.vert gpu_draw.frag static_scene.vert static_scene.frag gbuffer.vert gbuffer.frag gbuffer_bindless.frag static_indirect.vert static_indirect.comp static_indirect.frag fullscreen.vert deferred_direct.frag deferred_ibl.frag display_sdr.frag)
    set(source "${PROJECT_SOURCE_DIR}/shaders/${shader}")
    set(output "${shader_dir}/${shader}.spv")
    set(shader_target_env "vulkan1.3")
    set(shader_options)
    if(shader STREQUAL "heap_probe.comp")
        set(shader_target_env "vulkan1.4")
    endif()
    if(shader STREQUAL "deferred_ibl.frag")
        set(source "${PROJECT_SOURCE_DIR}/shaders/deferred_direct.frag")
        list(APPEND shader_options -DAR_ENVIRONMENT_LIGHTING=1)
    endif()
    if(shader STREQUAL "gbuffer_bindless.frag")
        set(source "${PROJECT_SOURCE_DIR}/shaders/gbuffer.frag")
        list(APPEND shader_options -DAR_BINDLESS_MATERIALS=1)
    endif()
    if(shader STREQUAL "static_indirect.frag")
        set(source "${PROJECT_SOURCE_DIR}/shaders/gbuffer.frag")
        list(APPEND shader_options -DAR_BINDLESS_MATERIALS=1 -DAR_STATIC_INDIRECT=1)
    endif()
    if(shader STREQUAL "gpu_cull.comp" OR shader STREQUAL "static_indirect.comp")
        # Use literal LocalSize, without enabling maintenance4 for this path.
        list(APPEND shader_options --target-spv=spv1.5)
    endif()
    add_custom_command(OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_dir}"
        COMMAND "${AR_GLSLC}" "--target-env=${shader_target_env}" ${shader_options} "-DAR_SET_IMAGE_COUNT=${binding_images}" "-DAR_SET_SAMPLER_COUNT=${binding_samplers}" "${source}" -o "${output}.tmp"
        COMMAND "${AR_SPIRV_VAL}" --target-env "${shader_target_env}" "${output}.tmp"
        COMMAND ${CMAKE_COMMAND} -E rename "${output}.tmp" "${output}"
        DEPENDS "${source}" "${binding_limits}" "${PROJECT_SOURCE_DIR}/shaders/static_indirect_scene.glsl" "${AR_GLSLC}" "${AR_SPIRV_VAL}"
        VERBATIM)
    set(deployed_shader "${triangle_runtime_shader_dir}/${shader}.spv")
    add_custom_command(OUTPUT "${deployed_shader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${triangle_runtime_shader_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy "${output}" "${deployed_shader}"
        DEPENDS "${output}"
        VERBATIM)
    list(APPEND triangle_shaders "${output}" "${deployed_shader}")
endforeach()
add_custom_target(ar_triangle_shaders DEPENDS ${triangle_shaders})
set_target_properties(ar_triangle_shaders PROPERTIES FOLDER "Examples")

set(triangle_showroom_source_dir "${PROJECT_SOURCE_DIR}/examples/triangle/content/showroom")
set(triangle_showroom_deploy_dir "${triangle_runtime_dir}/content/showroom")
set(triangle_showroom_text_files
    LICENSE.txt
    scene.json
    assets/floor.gltf
    assets/metallic_0.gltf
    assets/metallic_1.gltf
    assets/metallic_2.gltf
    assets/metallic_3.gltf
    assets/metallic_4.gltf
    assets/pillar.gltf
    assets/roughness_0.gltf
    assets/roughness_1.gltf
    assets/roughness_2.gltf
    assets/roughness_3.gltf
    assets/roughness_4.gltf
    assets/wall.gltf
)
set(triangle_showroom_outputs)
foreach(relative_file IN LISTS triangle_showroom_text_files)
    set(source_file "${triangle_showroom_source_dir}/${relative_file}")
    set(deployed_file "${triangle_showroom_deploy_dir}/${relative_file}")
    get_filename_component(deployed_parent "${deployed_file}" DIRECTORY)
    add_custom_command(OUTPUT "${deployed_file}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${deployed_parent}"
        COMMAND ${CMAKE_COMMAND} -E copy "${source_file}" "${deployed_file}"
        DEPENDS "${source_file}"
        VERBATIM)
    list(APPEND triangle_showroom_outputs "${deployed_file}")
endforeach()
set(triangle_showroom_generator "${triangle_showroom_source_dir}/generate_assets.py")
set(triangle_showroom_generated_assets
    "${triangle_showroom_deploy_dir}/assets/unit_box.bin"
    "${triangle_showroom_deploy_dir}/assets/unit_sphere.bin"
    "${triangle_showroom_deploy_dir}/assets/white.png"
    "${triangle_showroom_deploy_dir}/assets/floor_grid.png"
)
add_custom_command(OUTPUT ${triangle_showroom_generated_assets}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${triangle_showroom_deploy_dir}/assets"
    COMMAND "${Python3_EXECUTABLE}" -B "${triangle_showroom_generator}"
        --output-dir "${triangle_showroom_deploy_dir}/assets"
    DEPENDS "${triangle_showroom_generator}"
    VERBATIM)
list(APPEND triangle_showroom_outputs ${triangle_showroom_generated_assets})
add_custom_target(ar_triangle_content DEPENDS ${triangle_showroom_outputs})
set_target_properties(ar_triangle_content PROPERTIES FOLDER "Examples")

add_executable(ar_triangle main.cpp AppPaths.cpp)
find_package(Stb REQUIRED)
target_include_directories(ar_triangle SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
add_dependencies(ar_triangle ar_triangle_shaders ar_triangle_content)
target_link_libraries(ar_triangle PRIVATE ar_runtime ar_render)
target_compile_definitions(ar_triangle PRIVATE
    AR_TRIANGLE_OUTPUT_ROOT=u8"${CMAKE_BINARY_DIR}/sample-output/$<CONFIG>"
    AR_TRIANGLE_SHADER_DIR=u8"${shader_dir}"
)
set_target_properties(ar_triangle PROPERTIES
    FOLDER "Examples"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--showroom"
)
