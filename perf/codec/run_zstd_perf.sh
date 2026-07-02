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

#! /bin/bash


# Check if exactly 2 argument is provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <sw/qat> <zerocopy:true/false>"
    exit 1
fi

# Store the argument in a variable
method=$1
zerocopy=$2

# Display the input argument
echo "You choosed: method=$method, zerocopy=$zerocopy"

# compression levels
lvls=(1 2 3 4 5 6 7 8 9)

chunk_sizes=($((1*1024)) $((2*1024)) $((4*1024)) $((8*1024)) $((16*1024)) $((32*1024)) $((64*1024)) $((128*1024)))
zstd_perf_path=/data01/qlma/veSAL/build/src/codec/perf/zstd_perf

# non-streaming api test
# ZSTD_compress2
for chunk_size in "${chunk_sizes[@]}"; do
    for lvl in "${lvls[@]}"; do
        taskset -c 4 $zstd_perf_path --level=$lvl --chunk_size=$chunk_size --method=$method --streaming=false --zerocopy=$zerocopy
    done
done

# streaming api test
# ZSTD_compressStream2
# customer always use default ZSTD_c_maxBlockSize=128KB
for lvl in "${lvls[@]}"; do
    taskset -c 4 $zstd_perf_path --level=$lvl --chunk_size=$((128*1024)) --method=$method --streaming=true --zerocopy=$zerocopy
done