# Copyright (c) 2025 ByteDance Inc.
#
# This file is part of veSAL.
#
# veSAL is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

cmake_minimum_required(VERSION 3.13)

add_library(vesal_third_libs INTERFACE)

target_include_directories(vesal_third_libs INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/flat_hash_map
  ${CMAKE_CURRENT_SOURCE_DIR}/lz4/lib)

if(VESAL_ENABLE_SSL)
  find_package(OpenSSL REQUIRED)
  target_link_libraries(vesal_third_libs INTERFACE OpenSSL::Crypto)
endif()

find_library(NUMA_LIBRARY numa REQUIRED)
target_link_libraries(vesal_third_libs INTERFACE ${NUMA_LIBRARY})

set(BUILD_TESTING OFF CACHE BOOL "Disable third-party tests" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Disable googletest install rules" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "Build googlemock" FORCE)
add_subdirectory(googletest)

set(GFLAGS_BUILD_TESTING OFF CACHE BOOL "Disable gflags tests" FORCE)
set(GFLAGS_BUILD_PACKAGING OFF CACHE BOOL "Disable gflags packaging" FORCE)
set(GFLAGS_INSTALL_HEADERS OFF CACHE BOOL "Disable gflags install headers" FORCE)
set(GFLAGS_INSTALL_STATIC_LIBS OFF CACHE BOOL "Disable gflags install libraries" FORCE)
add_subdirectory(gflags)
if(TARGET gflags::gflags AND NOT TARGET gflags)
  add_library(gflags INTERFACE IMPORTED GLOBAL)
  target_link_libraries(gflags INTERFACE gflags::gflags)
endif()

target_link_libraries(vesal_third_libs INTERFACE gflags)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Disable spdlog examples" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "Disable spdlog tests" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "Disable spdlog benchmarks" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "Disable spdlog install rules" FORCE)
add_subdirectory(spdlog)
target_link_libraries(vesal_third_libs INTERFACE spdlog::spdlog)

set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "Disable libdeflate shared library" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "Disable libdeflate gzip tool" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "Disable libdeflate tests" FORCE)
add_subdirectory(libdeflate)
target_link_libraries(vesal_third_libs INTERFACE libdeflate::libdeflate_static)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "Disable zstd programs" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "Disable zstd shared library" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "Build zstd static library" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Disable zstd tests" FORCE)
add_subdirectory(zstd/build/cmake)
target_link_libraries(vesal_third_libs INTERFACE libzstd_static)

set(LZ4_BUILD_CLI OFF CACHE BOOL "Disable lz4 command line tools" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "Disable lz4c command line tool" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Disable shared third-party libraries" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build static third-party libraries" FORCE)
add_subdirectory(lz4/build/cmake)
target_link_libraries(vesal_third_libs INTERFACE lz4_static)

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Disable zlib examples" FORCE)
add_subdirectory(zlib)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/zlib/zconf.h.included
  AND NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/zlib/zconf.h)
  file(RENAME ${CMAKE_CURRENT_SOURCE_DIR}/zlib/zconf.h.included
          ${CMAKE_CURRENT_SOURCE_DIR}/zlib/zconf.h)
endif()
target_link_libraries(vesal_third_libs INTERFACE zlibstatic)

set(gperftools_build_benchmark OFF CACHE BOOL "Disable gperftools benchmarks" FORCE)
set(gperftools_build_minimal ON CACHE BOOL "Build minimal gperftools" FORCE)
set(gperftools_enable_libunwind OFF CACHE BOOL "Disable gperftools libunwind" FORCE)
add_subdirectory(gperftools)
if(TARGET tcmalloc_minimal_static AND NOT TARGET tcmalloc-static)
  add_library(tcmalloc-static ALIAS tcmalloc_minimal_static)
elseif(TARGET tcmalloc_static AND NOT TARGET tcmalloc-static)
  add_library(tcmalloc-static ALIAS tcmalloc_static)
endif()
