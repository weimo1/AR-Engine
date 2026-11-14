add_library(ar_render STATIC src/CullReference.cpp)
target_include_directories(ar_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ar_render PUBLIC ar_cxx23 ar_cxx_flags)
set_target_properties(ar_render PROPERTIES FOLDER "Engine")
