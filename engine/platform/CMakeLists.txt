find_package(glfw3 CONFIG REQUIRED)

add_library(ar_platform STATIC src/Window.cpp)
target_include_directories(ar_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ar_platform
    PUBLIC ar_core ar_cxx23 ar_cxx_flags
    PRIVATE glfw
)
set_target_properties(ar_platform PROPERTIES FOLDER "Engine")
