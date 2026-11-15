find_package(EnTT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(ar_scene STATIC src/StaticScene.cpp)
target_include_directories(ar_scene PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ar_scene
    PUBLIC ar_cxx23 ar_cxx_flags
    PRIVATE EnTT::EnTT nlohmann_json::nlohmann_json
)
set_target_properties(ar_scene PROPERTIES FOLDER "Engine")
