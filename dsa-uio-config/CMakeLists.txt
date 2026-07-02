# Add the library
add_library(lib_dsa_uio_config OBJECT
    device.c 
    log.c 
    user_device.c
    dsa_uio_config.c
)

target_include_directories(lib_dsa_uio_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Enable logging
target_compile_definitions(lib_dsa_uio_config PRIVATE ENABLE_LOGGING)

target_link_libraries(lib_dsa_uio_config PRIVATE ${DSA_UIO_CONFIG_COMPILE_OPTIONS})

# Add the executable
add_executable(test_executable test.cc)

target_link_libraries(test_executable PRIVATE lib_dsa_uio_config)

# Add additional compiler options
target_compile_options(test_executable PUBLIC -fpermissive -fPIC)

# Add the executable
add_executable(prepare_dsa_env prepare_dsa_env.cc)

target_link_libraries(prepare_dsa_env PRIVATE lib_dsa_uio_config)

# Add additional compiler options
target_compile_options(prepare_dsa_env PUBLIC -fpermissive -fPIC)

