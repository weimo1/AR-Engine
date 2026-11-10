find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(ar_core STATIC
    src/Error.cpp
    src/Diagnostics.cpp
)

target_include_directories(ar_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# spdlog and JSON are private implementation details; nothing from them appears in the
# public headers.
target_link_libraries(ar_core
    PUBLIC
        ar_cxx23
        ar_cxx_flags
    PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
)

if(AR_ENABLE_TEST_HOOKS)
    target_compile_definitions(ar_core PRIVATE AR_ENABLE_TEST_HOOKS=1)
    message(STATUS "ar: diagnostics test hooks enabled (test build only)")
endif()

ar_add_build_info(ar_core)

set_target_properties(ar_core PROPERTIES FOLDER "Engine")
