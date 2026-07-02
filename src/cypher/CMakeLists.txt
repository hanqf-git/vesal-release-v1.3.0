# Copyright (c) 2025 ByteDance Inc.
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

add_library(
  vesal_cypher_obj OBJECT
  cypher.cc
  qat_cypher_channel.cc
  sw_cypher_channel.cc
)
if(${VESAL_ENABLE_SSL})
    target_compile_definitions(vesal_cypher_obj PRIVATE VESAL_ENABLE_SSL)
endif()

target_link_libraries(vesal_cypher_obj PRIVATE vesal_compile_options vesal_includes
                          vesal_internal_includes vesal_third_libs)