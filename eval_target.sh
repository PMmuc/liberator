#!/bin/bash
# Usage: ./eval_target.sh [--docker] [--run-only] [target] [iterations]
#   --docker    run the analysis inside the docker container (docker/run_analysis.sh)
#               instead of locally (analysis.sh)
#   --run-only  skip fetching/building the library, pass --run-only to analysis.sh
cd "$(dirname "$0")"

USE_DOCKER=false
ANALYSIS_ARGS=()
ARGS=()
for arg in "$@"; do
  case "$arg" in
  --docker) USE_DOCKER=true ;;
  --run-only) ANALYSIS_ARGS+=(--run-only) ;;
  *) ARGS+=("$arg") ;;
  esac
done

TARGET=${ARGS[0]:-cjson}
NUM_IT=${ARGS[1]:-7}
TARGET_DIR=analysis/${TARGET}
mkdir -p test_results/dp/${TARGET}

for i in $(seq $NUM_IT); do
  LOG=test_results/dp/${TARGET}/${TARGET}_$i.log
  rm -f ${TARGET_DIR}/work/apipass/conditions.json
  if [ "$USE_DOCKER" = true ]; then
    SKIP_BUILD=1 TARGET=${TARGET} ANALYSIS_ARGS="${ANALYSIS_ARGS[*]}" \
      ./docker/run_analysis.sh >$LOG 2>&1
  else
    # Same as targets/start_analysis.sh, but on the host
    mkdir -p ${TARGET_DIR}
    { time ./analysis.sh ${TARGET} "${ANALYSIS_ARGS[@]}" >$LOG 2>&1; } \
      2>${TARGET_DIR}/${TARGET}_analysis_time.txt
  fi
  cat ${TARGET_DIR}/${TARGET}_analysis_time.txt
  cat ${TARGET_DIR}/${TARGET}_analysis_time.txt >>$LOG 2>/dev/null
done
