#!/usr/bin/env bash
set -euo pipefail

LOG_FILE="${QAT_REPLAY_LOG_FILE:-./replay_$(date +%Y%m%d_%H%M%S).log}"
# Mirror both stdout and stderr to terminal and a local log file.
exec > >(tee -a "${LOG_FILE}") 2>&1
echo "[replay] logging to ${LOG_FILE}"

export ICP_ROOT=/home/qiufeng/QAT/QAT20.L.1.2.30-00090_patch07070908
#export QAT_DC_TIMEOUT_DEBUG_DELAY_MSECS=3100
#export QAT_DC_TIMEOUT_DEBUG_DELAY_ONCE=1

LOOP_NUM="${1:-1000000}"
INPUT_PATH="${QAT_REPLAY_INPUT_PATH:-/home/qiufeng/QAT/bd_log0630/data00}"
INFLIGHT_NUM="${QAT_REPLAY_INFLIGHT_NUM:-64}"
MEM_POOL_MB="${QAT_REPLAY_MEM_POOL_MB:-8192}"
# Leave section name empty by default so QAT can auto-pick available SSL* sections.
QAT_SECTION_NAME="${QAT_REPLAY_QAT_SECTION_NAME:-SSL0}"

cmd=(
  numactl -N 0 ../../data_replay
  --input_path="${INPUT_PATH}"
  --replay_mode=per_file
  --inflight_num="${INFLIGHT_NUM}"
  --loop_num="${LOOP_NUM}"
  --mem_pool_mb="${MEM_POOL_MB}"
)

if [[ -n "${QAT_SECTION_NAME}" ]]; then
  cmd+=(--vesal_codec_qat_section_name="${QAT_SECTION_NAME}")
fi

time "${cmd[@]}"

unset QAT_DC_TIMEOUT_DEBUG_DELAY_MSECS
unset QAT_DC_TIMEOUT_DEBUG_DELAY_ONCE
