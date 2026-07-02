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

# Compile all ut test.

# ut need to access internal header
add_library(vesal_ut INTERFACE)
target_link_libraries(
  vesal_ut INTERFACE vesal_compile_options vesal_includes
                     vesal_internal_includes vesal_third_libs vesal)

function(vesal_test test_file)
  set(extra_args ${ARGN})

  get_filename_component(test_target_name "${test_file}" NAME_WE)
  set_source_files_properties(${test_file} PROPERTIES COMPILE_FLAGS
                                                      "-fno-access-control")

  add_executable("${test_target_name}" "${test_file}")
  target_link_libraries("${test_target_name}" GTest::gtest_main
                        GTest::gmock_main "${extra_args}")
  add_test(NAME "${test_target_name}" COMMAND "${test_target_name}")
endfunction(vesal_test)

vesal_test("timestamp_test.cc" vesal_ut)
vesal_test("object_pool_test.cc" vesal_ut)
vesal_test("qat_codec_engine_test.cc" vesal_ut)
vesal_test("qat_codec_test.cc" vesal_ut)
vesal_test("log_setting_test.cc" vesal_ut)
vesal_test("codec_test.cc" vesal_ut)
vesal_test("qat_session_test.cc" vesal_ut)
vesal_test("qat_buffer_test.cc" vesal_ut)
vesal_test("status_test.cc" vesal_ut)
vesal_test("memory_pool_helper_test.cc" vesal_ut)
vesal_test("memory_pool_test.cc" vesal_ut)
vesal_test("defer_test.cc" vesal_ut)
vesal_test("qat_error_handling_test.cc" vesal_ut)
vesal_test("sw_test.cc" vesal_ut)
vesal_test("dc_format_test.cc" vesal_ut)
vesal_test("scheduler_test.cc" vesal_ut)
vesal_test("chunk_pool_test.cc" vesal_ut)
vesal_test("data_flow_channel_test.cc" vesal_ut)
vesal_test("data_flow_engines_test.cc" vesal_ut)
vesal_test("qat_unit_manager_test.cc" vesal_ut)
vesal_test("qat_ha_test.cc" vesal_ut)
vesal_test("qat_poller_worker_test.cc" vesal_ut)
vesal_test("qat_poller_test.cc" vesal_ut)
vesal_test("qat_poller_manager_test.cc" vesal_ut)
vesal_test("qat_codec_shared_channel_test.cc" vesal_ut)
vesal_test("qat_codec_dedicated_channel_test.cc" vesal_ut)
vesal_test("mpsc_queue_test.cc" vesal_ut)
vesal_test("c_api_test.cc" vesal_ut)
vesal_test("qat_cypher_channel_test.cc" vesal_ut)
vesal_test("req_ring_queue_test.cc" vesal_ut)
vesal_test("vesal_test.cc" vesal_ut)
# Only enable kae ut on KP machine
if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    vesal_test("kae_codec_test" vesal_ut)
endif()
# Check if the system is x86 (32-bit or 64-bit)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "i686")
    vesal_test("dsa_dispatcher_test.cc" vesal_ut)
    vesal_test("dsa_ctx_factory_test.cc" vesal_ut)
    vesal_test("dsa_schedule_strategy_test.cc" vesal_ut)
endif()
if (VESAL_ENABLE_ERR_SIM)
    vesal_test("uds_listener_test.cc" vesal_ut)
    vesal_test("err_simulation_test.cc" vesal_ut)
endif()
if (${VESAL_ENABLE_ZSTD})
    vesal_test("zstd_test.cc" vesal_ut)
endif()
