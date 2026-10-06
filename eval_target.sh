#!/bin/bash
# Usage: ./eval_target.sh [--docker] [target] [iterations]
#   --docker  run the analysis inside the docker container (docker/run_analysis.sh)
#             instead of locally (analysis.sh)
cd "$(dirname "$0")"
mkdir -p test_results/dp

USE_DOCKER=false
ARGS=()
for arg in "$@"; do
  case "$arg" in
  --docker) USE_DOCKER=true ;;
  *) ARGS+=("$arg") ;;
  esac
done

TARGET=${ARGS[0]:-cjson}
NUM_IT=${ARGS[1]:-7}
TARGET_DIR=analysis/${TARGET}

for i in $(seq $NUM_IT); do
  LOG=test_results/dp/${TARGET}/${TARGET}_$i.log
  rm -f ${TARGET_DIR}/work/apipass/conditions.json
  if [ "$USE_DOCKER" = true ]; then
    SKIP_BUILD=1 TARGET=${TARGET} ./docker/run_analysis.sh >$LOG 2>&1
  else
    # Same as targets/start_analysis.sh, but on the host
    mkdir -p ${TARGET_DIR}
    { time ./analysis.sh ${TARGET} >$LOG 2>&1; } \
      2>${TARGET_DIR}/${TARGET}_analysis_time.txt
  fi
  cat ${TARGET_DIR}/${TARGET}_analysis_time.txt
  cat ${TARGET_DIR}/${TARGET}_analysis_time.txt >>$LOG 2>/dev/null
done
