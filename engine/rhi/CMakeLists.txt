find_package(volk CONFIG REQUIRED)

add_library(ar_rhi STATIC
    src/vulkan/RhiSession.cpp
)

target_include_directories(ar_rhi
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan"
)

target_link_libraries(ar_rhi
    PUBLIC
        ar_core
        ar_cxx23
        ar_cxx_flags
    PRIVATE
        volk::volk
)
