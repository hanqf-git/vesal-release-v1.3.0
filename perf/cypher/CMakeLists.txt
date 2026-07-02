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

add_executable(cy_perf cy_perf.cc)

target_link_libraries(cy_perf PRIVATE vesal_compile_options vesal
                                          tcmalloc-static gflags vesal_perf_includes)

if(${VESAL_ENABLE_BYTE_METRICS})
  target_compile_definitions(cy_perf PRIVATE VESAL_ENABLE_BYTE_METRICS)
  target_link_libraries(cy_perf PRIVATE byte::metrics_holder unwind)
endif()
