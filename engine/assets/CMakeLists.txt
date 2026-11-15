find_package(fastgltf CONFIG REQUIRED)
find_package(Stb REQUIRED)

add_library(ar_assets STATIC src/StaticAssetCache.cpp)
target_include_directories(ar_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(ar_assets SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
target_link_libraries(ar_assets
    PUBLIC ar_cxx23 ar_cxx_flags
    PRIVATE fastgltf::fastgltf
)
set_target_properties(ar_assets PROPERTIES FOLDER "Engine")
