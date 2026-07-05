numactl -N 2 ../../data_replay \
  --input_path=/home/qiufeng/QAT/bd_log0630/data00 \
  --replay_mode=per_file \
  --inflight_num=64 \
  --loop_num=$1 \
  --mem_pool_mb=8192 \
  --vesal_codec_qat_section_name=SSL2
