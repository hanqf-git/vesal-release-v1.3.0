numactl -N 1 ../../data_replay \
  --input_path=/home/qiufeng/QAT/bd_log/ \
  --replay_mode=per_file \
  --inflight_num=128 \
  --loop_num=$1 \
  --vesal_codec_qat_section_name=SSL0
