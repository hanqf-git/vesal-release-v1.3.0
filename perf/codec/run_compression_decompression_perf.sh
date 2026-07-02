#! /bin/bash

inflight_nums=(4 16 64)
source_sizes=($((1*1024)) $((2*1024)) $((4*1024)) $((8*1024)) $((16*1024)) $((32*1024)) $((64*1024)))
background_log=background_log.txt
rm -f $background_log

compression_perf_file_path=compression-perf.txt
compression_perf_with_decompression_file=compression-perf-with-decompression.txt

CompressionPerf()
{
  local output_file="./$1"
  local source_size=0
  local inflight_num=0
  for source_size in "${source_sizes[@]}"; do
    for inflight_num in "${inflight_nums[@]}"; do
        echo "Running compression perf with inflight_num=$inflight_num, source_size=$source_size"
        taskset -c 0 ./perf_simple --channel_num=1 --total_data_per_thread=20 --memory_type=register_2mb --inflight_num=$inflight_num --source_size=$source_size --decompression_perf=false --vesal_codec_qat_section_name=SSL0 >> $output_file
        echo "Finished compression perf with inflight_num=$inflight_num, source_size=$source_size, appending results to $output_file"
    done
  done       
}

# compression perf without other traffic
rm -f $compression_perf_file_path
CompressionPerf $compression_perf_file_path

# compression perf with decompression traffic
rm -f $compression_perf_with_decompression_file
for source_size in "${source_sizes[@]}"; do
    for inflight_num in "${inflight_nums[@]}"; do
        # run background decompression
        taskset -c 1 ./perf_simple --channel_num=1 --total_data_per_thread=2000000 --memory_type=register_2mb --inflight_num=$inflight_num --source_size=$source_size --compression_perf=false --vesal_codec_qat_section_name=SSL1 >>$background_log 2>&1 &
        # get background decompression pid
        PID=$!
        # let the background process finish setup and keep running decompression
        echo "Running background decompression with inflight_num=$inflight_num, source_size=$source_size, PID=$PID"
        sleep 10
        echo "Slept 10 seconds to let the background process finish setup and keep running decompression"

        # compression perf
        CompressionPerf $compression_perf_with_decompression_file

        # kill background decompression
        if kill -0 $PID 2>/dev/null; then
            # background process is still running as expected
            kill -9 $PID
            echo "Killed background process"
        else
            echo "Background process with PID $PID is dead earlier than expected. try bigger 'total_data_per_thread'."
        fi

        # it will cause error if start next background process too soon
        sleep 2
    done
done

decompression_perf_file_path=decompression-perf.txt
decompression_perf_with_compression_file=decompression-perf-with-compression.txt

DecompressionPerf()
{
  local output_file="./$1"
  local source_size=0
  local inflight_num=0
  for source_size in "${source_sizes[@]}"; do
    for inflight_num in "${inflight_nums[@]}"; do
        echo "Running decompression perf with inflight_num=$inflight_num, source_size=$source_size"
        taskset -c 0 ./perf_simple --channel_num=1 --total_data_per_thread=20 --memory_type=register_2mb --inflight_num=$inflight_num --source_size=$source_size --compression_perf=false --vesal_codec_qat_section_name=SSL0 >> $output_file
        echo "Finished decompression perf with inflight_num=$inflight_num, source_size=$source_size, appending results to $output_file"
    done
  done       
}

# decompression perf without other traffic
rm -f $decompression_perf_file_path
DecompressionPerf $decompression_perf_file_path

# decompression perf with compression traffic
rm -f $decompression_perf_with_compression_file
for source_size in "${source_sizes[@]}"; do
    for inflight_num in "${inflight_nums[@]}"; do
        # run background compression
        taskset -c 1 ./perf_simple --channel_num=1 --total_data_per_thread=2000000 --memory_type=register_2mb --inflight_num=$inflight_num --source_size=$source_size --decompression_perf=false --vesal_codec_qat_section_name=SSL1 >>$background_log 2>&1 &
        # get background compression pid
        PID=$!
        # let the background process finish setup and keep running compression
        echo "Running background compression with inflight_num=$inflight_num, source_size=$source_size, PID=$PID"
        sleep 10
        echo "Slept 10 seconds to let the background process finish setup and keep running compression"

        # decompression perf
        DecompressionPerf $decompression_perf_with_compression_file

        # kill background compression
        if kill -0 $PID 2>/dev/null; then
            # background process is still running as expected
            kill -9 $PID
            echo "Killed background process"
        else
            echo "Background process with PID $PID is dead earlier than expected. try bigger 'total_data_per_thread'."
        fi

        # it will cause error if start next background process too soon
        sleep 2
    done
done
