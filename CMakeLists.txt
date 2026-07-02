# Copyright (c) 2023 ByteDance Inc.
#
# This file is part of veSAL.
#
# veSAL is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# veSAL is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with veSAL. If not, see <https://www.gnu.org/licenses/>.

include("CheckCSourceRuns")

cmake_minimum_required(VERSION 3.13)

# Prevent multiple inclusions of this project
if(TARGET vesal_compile_options)
    message(STATUS "veSAL is already included, skipping.")
    return()
endif()

project(veSAL LANGUAGES C CXX)

message("CMAKE_CXX_COMPILER_ID : ${CMAKE_CXX_COMPILER_ID}")
message("CMAKE_CXX_COMPILER_VERSION : ${CMAKE_CXX_COMPILER_VERSION}")

if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Override policy for CMake ≥4.0" FORCE)
endif()

option(VESAL_ENABLE_ASAN "Build enable asan or not" OFF)
option(VESAL_BUILD_TESTS "Build unit test or not" OFF)
option(VESAL_BUILD_PERF  "Build perf tool or not" OFF)
option(VESAL_ENABLE_GCOV "Build with coverage analysis" OFF)
option(VESAL_ENABLE_ZSTD "Build with zstd related functionality" OFF)
option(VESAL_ENABLE_LZ4IPP "Build with lz4 ipp feature" OFF)
option(VESAL_ENABLE_ERR_SIM "Build with error simulation enabled" OFF)
option(VESAL_BUILD_BENCH "Build benchmarks or not" OFF)
option(VESAL_ENABLE_SSL "Build openssl or not" ON)
option(VESAL_ENABLE_KAE "Build KAE codec support or not" ON)

if(VESAL_ENABLE_KAE)
  find_path(KAEZIP_INCLUDE_DIR kaezip.h)
  if(KAEZIP_INCLUDE_DIR)
    message(STATUS "Found kaezip.h: ${KAEZIP_INCLUDE_DIR}")
  else()
    message(WARNING "kaezip.h not found, disabling KAE codec support")
    set(VESAL_ENABLE_KAE OFF CACHE BOOL "Build KAE codec support or not" FORCE)
  endif()
endif()

# avoid being overwritten.
set(ORIGINAL_CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "-msse4.2")
check_c_source_runs("
    #include <immintrin.h>
    int main() {
        _mm_crc32_u32(0x1234, 0x5678);
        return 0;
    }"
    VESAL_HAVE_SSE4_2)
set(CMAKE_REQUIRED_FLAGS "-mpclmul")
check_c_source_runs("
    #include <immintrin.h>
    int main() {
        __m128i va = _mm_set_epi32(0x1234, 0x5678, 0x9abc, 0xdef0);
        __m128i vb = _mm_set_epi32(0xaaaa, 0xbbbb, 0xcccc, 0xdddd);
        _mm_clmulepi64_si128(va, vb, 0);
        return 0;
    }"
    VESAL_HAVE_PCLMULQDQ)
message("-- Detected VESAL_HAVE_SSE4_2=${VESAL_HAVE_SSE4_2}")
message("-- Detected VESAL_HAVE_PCLMULQDQ=${VESAL_HAVE_PCLMULQDQ}")
if (VESAL_HAVE_SSE4_2)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DVESAL_HAVE_SSE4_2")
endif ()
if (VESAL_HAVE_PCLMULQDQ)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DVESAL_HAVE_PCLMULQDQ")
endif ()
set(CMAKE_REQUIRED_FLAGS "${ORIGINAL_CMAKE_REQUIRED_FLAGS}")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# TODO(sjj): combine all flags into here
add_library(vesal_compile_options INTERFACE)

target_compile_options(vesal_compile_options INTERFACE
    -Wall -Werror -ggdb3 -fno-omit-frame-pointer
)

if (VESAL_ENABLE_ERR_SIM)
    target_compile_options(vesal_compile_options INTERFACE -DVESAL_ENABLE_ERR_SIM)
endif()

add_library(vesal_internal_includes INTERFACE)

if(VESAL_ENABLE_KAE)
  target_compile_options(vesal_compile_options INTERFACE -DVESAL_ENABLE_KAE)
  target_include_directories(vesal_internal_includes INTERFACE ${KAEZIP_INCLUDE_DIR})
endif()

# Insert GIT_HASH GIT_BRANCH into include/vesal/version.h
find_package(Git)
execute_process(COMMAND ${GIT_EXECUTABLE} log -1 --format=%H
  WORKING_DIRECTORY ${veSAL_SOURCE_DIR}
  OUTPUT_VARIABLE  VESAL_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
  WORKING_DIRECTORY ${veSAL_SOURCE_DIR}
  OUTPUT_VARIABLE VESAL_GIT_BRANCH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/common/vesal_version.h.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/vesal/vesal_version.h" @ONLY
)

if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(CMAKE_CXX_FLAGS "-mcpu=generic+crc ${CMAKE_CXX_FLAGS}")
endif()

if(VESAL_ENABLE_ASAN)
  message(STATUS "build vesal with address sanitizer")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
  add_compile_definitions(VESAL_ENABLE_ASAN)
endif()

if (VESAL_ENABLE_GCOV)
    message(STATUS "enable coverage analysis with gcov")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
endif()

add_subdirectory(third)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-9]86")
  if(NOT TARGET lib_dsa_uio_config)
    target_include_directories(vesal_internal_includes
          INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dsa-uio-config/include/)
    # Get the compile options from vesal_compile_options and add them to DSA_UIO_CONFIG_COMPILE_OPTIONS
    get_target_property(VESAL_COMPILE_OPTIONS vesal_compile_options INTERFACE_COMPILE_OPTIONS)
    set(DSA_UIO_CONFIG_COMPILE_OPTIONS ${DSA_UIO_CONFIG_COMPILE_OPTIONS} ${VESAL_COMPILE_OPTIONS})
    add_subdirectory(dsa-uio-config)
    target_link_libraries(vesal_third_libs INTERFACE lib_dsa_uio_config)
  endif()
endif()

add_subdirectory(dummy_driver)

# Exported targets: vesal_include
add_subdirectory(include)

add_subdirectory(src)

if(VESAL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

if(VESAL_BUILD_BENCH)
  add_subdirectory(benchmark)
endif()

add_subdirectory(example)

add_subdirectory(tools)

if(${VESAL_BUILD_PERF})
  add_subdirectory(perf)
endif()