add_executable(ar_triangle main.cpp)
target_link_libraries(ar_triangle PRIVATE ar_runtime)
target_compile_definitions(ar_triangle PRIVATE
    AR_TRIANGLE_OUTPUT_ROOT=u8"${CMAKE_BINARY_DIR}/sample-output/$<CONFIG>"
)
set_target_properties(ar_triangle PROPERTIES
    FOLDER "Examples"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
