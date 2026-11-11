foreach(stage IN ITEMS vert frag)
    set(shader "${SHADER_DIR}/triangle.${stage}.spv")
    if(NOT EXISTS "${shader}")
        message(FATAL_ERROR "Missing built shader: ${shader}")
    endif()
    execute_process(COMMAND "${VALIDATOR}" --target-env vulkan1.3 "${shader}"
        COMMAND_ERROR_IS_FATAL ANY)
endforeach()
