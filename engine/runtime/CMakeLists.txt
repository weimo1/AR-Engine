find_package(nlohmann_json CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

add_library(ar_runtime STATIC
    src/CapabilityJson.cpp
    src/ObservationSettings.cpp
    src/RuntimeSession.cpp
    src/StaticSceneSession.cpp
)

target_include_directories(ar_runtime
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# rhi value types travel through the public header, so ar_rhi is a PUBLIC dependency; the JSON
# library stays private because capability_json crosses the boundary as text.
target_link_libraries(ar_runtime
    PUBLIC
        ar_core
        ar_platform
        ar_rhi
        ar_render
        ar_cxx23
        ar_cxx_flags
    PRIVATE
        nlohmann_json::nlohmann_json
        ar_assets
        ar_scene
        glm::glm
)

set_target_properties(ar_runtime PROPERTIES FOLDER "Engine")
