#!/bin/bash
# Usage: ./eval_target.sh [--run-only] [target] [iterations]
#   --run-only  skip fetching/building the library, reuse the previous build
#               in analysis/<target>/work and only rerun the analysis
cd "$(dirname "$0")"

ANALYSIS_ARGS=()
ARGS=()
for arg in "$@"; do
  case "$arg" in
  --run-only) ANALYSIS_ARGS+=(--run-only) ;;
  *) ARGS+=("$arg") ;;
  esac
done

TARGET=${ARGS[0]:-cjson}
NUM_IT=${ARGS[1]:-1}

mkdir -p test_results/base/${TARGET}

for i in $(seq $NUM_IT); do
  # delete conditions.json otherwise run_analysis skips target
  docker run --rm --user root -v "$PWD:/w" libpp-analysis-org \
    rm -f /w/analysis/${TARGET}/work/apipass/conditions.json
  (cd docker && SKIP_BUILD=1 TARGET=${TARGET} ./run_analysis.sh "${ANALYSIS_ARGS[@]}") \
    >test_results/base/${TARGET}/${TARGET}_$i.log 2>&1
  cat analysis/${TARGET}/${TARGET}_analysis_time.txt \
    >>test_results/base/${TARGET}/${TARGET}_$i.log 2>/dev/null
done
