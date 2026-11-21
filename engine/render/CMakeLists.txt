find_package(glm CONFIG REQUIRED)

add_library(ar_render STATIC src/CullReference.cpp src/EnvironmentLighting.cpp src/OpaqueMaterial.cpp src/PresentationGraph.cpp src/RenderGraph.cpp src/StaticSceneRenderer.cpp)
target_include_directories(ar_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ar_render
    PUBLIC ar_core ar_cxx23 ar_cxx_flags ar_rhi
    PRIVATE ar_assets glm::glm
)
set_target_properties(ar_render PROPERTIES FOLDER "Engine")
