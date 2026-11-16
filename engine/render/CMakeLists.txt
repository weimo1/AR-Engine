find_package(glm CONFIG REQUIRED)

add_library(ar_render STATIC src/CullReference.cpp src/OpaqueMaterial.cpp src/StaticSceneRenderer.cpp)
target_include_directories(ar_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ar_render
    PUBLIC ar_core ar_cxx23 ar_cxx_flags
    PRIVATE ar_assets ar_rhi glm::glm
)
set_target_properties(ar_render PROPERTIES FOLDER "Engine")
