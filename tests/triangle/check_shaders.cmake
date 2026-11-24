foreach(name IN ITEMS triangle.vert triangle.frag gpu_cull.comp shadow_direct.vert shadow_indirect.vert deferred_direct.frag deferred_ibl.frag deferred_shadow.frag deferred_ibl_shadow.frag)
    set(shader "${SHADER_DIR}/${name}.spv")
    if(NOT EXISTS "${shader}")
        message(FATAL_ERROR "Missing built shader: ${shader}")
    endif()
    execute_process(COMMAND "${VALIDATOR}" --target-env vulkan1.3 "${shader}"
        COMMAND_ERROR_IS_FATAL ANY)
endforeach()
