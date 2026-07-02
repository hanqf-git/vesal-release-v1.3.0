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

set(vesal_common_sources
  log_setting.cc
  object_pool.cc
  memory_pool.cc
  status.cc
  memory_pool_helper.cc
  vesal.cc
  timer_manager.cc
  metrics.cc
  scheduler.cc
  chunk_pool.cc
  checksum_impl.cc
  qat/qat_hardware_api_wrapper.cc
  qat/qat_session.cc
  qat/qat_unit_manager.cc
  qat/qat_unit.cc
  qat/qat_buffer.cc
  qat/qat_util.cc
)

if(VESAL_ENABLE_ERR_SIM)
  list(APPEND vesal_common_sources err_simulation.cc uds_listener.cc)
endif()

add_library(
  vesal_common_obj OBJECT ${vesal_common_sources})

target_link_libraries(
  vesal_common_obj PRIVATE vesal_compile_options vesal_includes
                           vesal_third_libs vesal_internal_includes)
          