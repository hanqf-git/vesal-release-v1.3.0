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

cmake_minimum_required(VERSION 3.13)

add_library(vesal_includes INTERFACE)

target_include_directories(vesal_includes INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}")
# Using vesal also introduces some headers from third party libraries.
# Export them selectively so that the users can build vesal independently.
target_link_libraries(vesal_includes INTERFACE gflags)
target_compile_definitions(vesal_includes INTERFACE ZSTD_STATIC_LINKING_ONLY)
target_include_directories(vesal_includes INTERFACE ${PROJECT_SOURCE_DIR}/third/zstd/lib)
