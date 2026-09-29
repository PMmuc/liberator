#!/bin/bash
# Usage: ./eval_target.sh [target] [runs]    (default: cjson 7)
cd "$(dirname "$0")"
mkdir -p test_results/base

TARGET=${1:-cjson}
NUM_IT=${2:-7}

for i in $(seq $NUM_IT); do
  # conditions.json belongs to root (the container runs as root), so delete it
  # from a container; otherwise run_analysis.sh skips the target
  docker run --rm --user root -v "$PWD:/w" libpp-analysis-org \
    rm -f /w/analysis/${TARGET}/work/apipass/conditions.json
  (cd docker && SKIP_BUILD=1 TARGET=${TARGET} ./run_analysis.sh) \
    >test_results/base/${TARGET}_$i.log 2>&1
done
