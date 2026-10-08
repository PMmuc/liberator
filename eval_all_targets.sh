#!/bin/bash
# Run eval_target.sh sequentially for every target.
#
#   ./eval_all_targets.sh [NUM_IT] [target ...]
#
# NUM_IT defaults to 7 (as in eval_target.sh); targets default to the
# evaluation projects of fuzzing_campaigns/campaign_configuration.sh.
# The analysis image is built once up front; set SKIP_BUILD=1 to reuse it.
# Per-iteration logs go to test_results/base/<target>_<i>.log, a summary to
# test_results/base/summary.txt.
cd "$(dirname "$0")"

NUM_IT=${1:-7}
shift || true

if [ "$#" -gt 0 ]; then
  TARGETS=("$@")
else
  TARGETS=("cpu_features" "libtiff" "minijail" "pthreadpool" "libaom" "libvpx"
    "libhtp" "libpcap" "c-ares" "zlib" "cjson" "libdwarf" "libsndfile"
    "libplist" "libucl")
fi

# stop the running analysis container on Ctrl-C, otherwise it keeps going
current=""
trap 'echo "[INFO] interrupted"; [ -n "$current" ] &&
  docker rm -f "libpp-analysis-org-$current" >/dev/null 2>&1; exit 130' INT TERM

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "[INFO] building analysis image"
  (cd docker && BUILD_ONLY=1 ./run_analysis.sh) || {
    echo "[ERROR] image build failed"
    exit 1
  }
fi

mkdir -p test_results/base
SUMMARY=test_results/base/summary.txt
echo "# $(date -Iseconds) NUM_IT=$NUM_IT" >>"$SUMMARY"

for t in "${TARGETS[@]}"; do
  if [ ! -f "targets/$t/analysis.sh" ]; then
    echo "[WARN] $t: no targets/$t/analysis.sh, skipping" | tee -a "$SUMMARY"
    continue
  fi

  current=$t
  echo "[INFO] $(date +%T) $t: $NUM_IT iterations"
  start=$(date +%s)
  ./eval_target.sh "$t" "$NUM_IT"
  elapsed=$(($(date +%s) - start))

  # eval_target.sh does not propagate failures, so check each log for the
  # extractor's last message and its profiling output
  ok=0
  for i in $(seq "$NUM_IT"); do
    grep -q "struct data layout done" "test_results/base/${t}_$i.log" 2>/dev/null &&
      ok=$((ok + 1))
  done
  prof=$(grep -c "Time Breakdown" "test_results/base/${t}_1.log" 2>/dev/null)

  printf "%-14s ok=%d/%d time=%ds profiling_blocks(it1)=%s\n" \
    "$t" "$ok" "$NUM_IT" "$elapsed" "${prof:-0}" | tee -a "$SUMMARY"
done
current=""

echo "[INFO] done, summary in $SUMMARY"
