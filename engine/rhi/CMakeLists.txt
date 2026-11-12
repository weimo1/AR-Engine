find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

add_library(ar_rhi STATIC
    src/vulkan/Presentation.cpp
    src/vulkan/RhiSession.cpp
    src/vulkan/resources/DeviceResources.cpp
    src/vulkan/resources/Transfers.cpp
    src/vulkan/resources/Vma.cpp
)

target_compile_definitions(ar_rhi PRIVATE
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
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
        GPUOpen::VulkanMemoryAllocator
)

set_target_properties(ar_rhi PROPERTIES FOLDER "Engine")
