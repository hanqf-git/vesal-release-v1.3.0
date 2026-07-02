# Copyright (c) 2023 ByteDance Inc.
#
# This file is part of veSAL.
#
# veSAL is free software: you can redistribute it and/or modify it under the
# terms of the GNU Lesser General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option) any
# later version.
#
# veSAL is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
# A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
# details.
#
# You should have received a copy of the GNU Lesser General Public License along
# with veSAL. If not, see <https://www.gnu.org/licenses/>.

# follow vesal's requirement
cmake_minimum_required(VERSION 3.13)

target_include_directories(
  vesal_internal_includes
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/quickassist/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/quickassist/include/dc
    ${CMAKE_CURRENT_SOURCE_DIR}/include/quickassist/include/lac
    ${CMAKE_CURRENT_SOURCE_DIR}/include/quickassist/lookaside/access_layer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(qat_dummy_driver_obj OBJECT src/qat_dummy_driver.c)
target_link_libraries(qat_dummy_driver_obj PRIVATE vesal_compile_options
                                                   vesal_internal_includes)

if(VESAL_ENABLE_KAE)
  add_library(kae_dummy_driver_obj OBJECT src/kae_dummy_driver.cc)
  target_link_libraries(kae_dummy_driver_obj PRIVATE vesal_compile_options vesal_internal_includes vesal_third_libs)
endif()

add_library(qat_dummy_driver STATIC $<TARGET_OBJECTS:qat_dummy_driver_obj>)
target_link_libraries(qat_dummy_driver PRIVATE dl)

add_subdirectory(test EXCLUDE_FROM_ALL)
