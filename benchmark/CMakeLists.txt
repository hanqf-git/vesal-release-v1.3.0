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

# Compile all vesal benchmarks

add_library(vesal_bench INTERFACE)
target_link_libraries(
  vesal_bench INTERFACE vesal_compile_options vesal_includes
                     vesal_internal_includes vesal_third_libs vesal benchmark::benchmark)

function(benchmark bench_file)
  get_filename_component(test_target_name "${bench_file}" NAME_WE)
  set_source_files_properties(${bench_file} PROPERTIES COMPILE_FLAGS
                                                      "-fno-access-control")

  add_executable("${test_target_name}" "${bench_file}")
  target_link_libraries("${test_target_name}" vesal_bench)
endfunction(benchmark)

benchmark("chunkpool_bench.cc")
benchmark("cpu_crc32_bench.cc")
benchmark("cpu_memcpy_bench.cc")
