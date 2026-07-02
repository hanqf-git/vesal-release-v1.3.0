# Copyright (c) 2024 ByteDance Inc.
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

set(DATA_FLOW_SOURCES
    data_flow_request.cc
    data_flow_channel_impl.cc
    sw/data_flow_sw_engine.cc
    data_flow.cc)

# Check if the system is x86 (32-bit or 64-bit)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "i686")
    list(
      APPEND DATA_FLOW_SOURCES 
      dsa/data_flow_dsa_engine.cc
      dsa/dsa_ctx_factory.cc
      dsa/dsa_dispatcher.cc
      dsa/schedule_strategy.cc)
endif()

add_library(vesal_data_flow_obj OBJECT ${DATA_FLOW_SOURCES})

target_link_libraries(
  vesal_data_flow_obj PRIVATE vesal_compile_options vesal_includes
                           vesal_third_libs vesal_internal_includes)
