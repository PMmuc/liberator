#!/bin/bash
cd "$(dirname "$0")"

TARGET=${1:-cjson}
NUM_IT=${2:-1}

mkdir -p test_results/base/${TARGET}

for i in $(seq $NUM_IT); do
  # delete conditions.json otherwise run_analysis skips target
  docker run --rm --user root -v "$PWD:/w" libpp-analysis-org \
    rm -f /w/analysis/${TARGET}/work/apipass/conditions.json
  (cd docker && SKIP_BUILD=1 TARGET=${TARGET} ./run_analysis.sh) \
    >test_results/base/${TARGET}/${TARGET}_$i.log 2>&1
  cat analysis/${TARGET}/${TARGET}_analysis_time.txt \
    >>test_results/base/${TARGET}_$i.log 2>/dev/null
done
