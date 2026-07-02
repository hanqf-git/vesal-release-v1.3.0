#! /bin/bash
# 1. Change "VESAL_MAX_SGL_NUM" to 64 for testing usage
# 2. Run "./build.sh" in vesal's root directory to successfully build "perf_simple"
# 3. Run "bash ./src/codec/perf/run_sgl_perf.sh" in vesal's root directory

sgl_sizes=(1 2 4 8 16 32 64)
source_sizes=(1024 $((2*1024)) $((4*1024)) $((8*1024)) $((16*1024)) $((32*1024)) $((64*1024)))

# Loop through each combination
for source_size in "${source_sizes[@]}"; do
    for sgl_size in "${sgl_sizes[@]}"; do
        # the dst size is not greater than 2mb-hugepage size
        # assume the dst size is up to twice the input size
        if (( 2 * $source_size * $sgl_size <= 2 * 1024 * 1024 )); then
            ./perf_simple --channel_num=1 --total_data_per_thread=20 --memory_type=register_2mb --inflight_num=64 --source_size=$source_size --sgl_size=$sgl_size
        fi
    done
done