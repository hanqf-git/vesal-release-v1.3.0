add_library(c_api_obj OBJECT c_api_vesal.cc c_api_cypher.cc)

target_link_libraries(
    c_api_obj PRIVATE vesal_compile_options vesal_includes
                          vesal_internal_includes vesal_third_libs)
