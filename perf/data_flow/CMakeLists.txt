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

add_executable(data_flow_perf data_flow_perf.cc)

target_link_libraries(data_flow_perf PRIVATE vesal_compile_options vesal
                                             vesal_perf_includes gflags tcmalloc-static unwind)

if(${VESAL_ENABLE_BYTE_METRICS})
  target_compile_definitions(data_flow_perf PRIVATE VESAL_ENABLE_BYTE_METRICS)
  target_link_libraries(data_flow_perf PRIVATE byte::metrics_holder unwind)
endif()

