find_package(volk CONFIG REQUIRED)

add_library(ar_rhi STATIC
    src/vulkan/RhiSession.cpp
)

target_include_directories(ar_rhi
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan"
)

if(AR_ENABLE_TEST_HOOKS)
    target_compile_definitions(ar_rhi PRIVATE AR_ENABLE_TEST_HOOKS=1)
    message(STATUS "ar: rhi test hooks enabled (quarantine and call counters are observable)")
endif()

target_link_libraries(ar_rhi
    PUBLIC
        ar_core
        ar_cxx23
        ar_cxx_flags
    PRIVATE
        volk::volk
)
