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

#! /bin/bash

io_depths=(1 2 4 8 16 32 64 128)
chunk_sizes=($((4*1024)) $((8*1024)) $((16*1024)) $((32*1024)) $((64*1024)) $((128*1024)))

# Loop through each combination and echo the values
for io_depth in "${io_depths[@]}"; do
    for chunk_size in "${chunk_sizes[@]}"; do
        taskset -c 0 ./build/src/data_flow/perf/data_flow_perf --chunk_size=$chunk_size --io_depth=$io_depth --op=1
    done
done

for io_depth in "${io_depths[@]}"; do
    for chunk_size in "${chunk_sizes[@]}"; do
        taskset -c 0 ./build/src/data_flow/perf/data_flow_perf --chunk_size=$chunk_size --io_depth=$io_depth --op=2
    done
done