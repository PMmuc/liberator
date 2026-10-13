#!/bin/bash
#
# Run ./eval_target.sh for every target in targets/ (except simple_connection),
# one after another. A failing target does not stop the loop.
#
# Usage: ./eval_all_targets.sh [--docker] [iterations]
#   e.g. ./eval_all_targets.sh --docker 7
#
# Restrict to specific targets with:  TARGETS="zlib cjson" ./eval_all_targets.sh
#
# If analysis/<target>/repo already exists, the library build is skipped and
# the analysis is run with --run-only.
#
set -u
cd "$(dirname "$0")"

EXCLUDE="simple_connection"

DOCKER_FLAG=()
NUM_IT=()
for arg in "$@"; do
  case "$arg" in
  --docker) DOCKER_FLAG=(--docker) ;;
  *) NUM_IT=("$arg") ;;
  esac
done

targets=()
if [ -n "${TARGETS:-}" ]; then
  read -ra targets <<<"$TARGETS"
else
  for target_dir in targets/*/; do
    target=$(basename "$target_dir")
    [ "$target" = "$EXCLUDE" ] && continue
    [ -f "$target_dir/config.sh" ] && targets+=("$target")
  done
fi

echo "[INFO] ${#targets[@]} targets: ${targets[*]}"

failed=()
for target in "${targets[@]}"; do
  echo "[INFO] === $target ($(date '+%F %T')) ==="
  run_only=()
  if [ -d "analysis/$target/repo" ]; then
    echo "[INFO] analysis/$target/repo exists, skipping build (--run-only)"
    run_only=(--run-only)
  fi
  if ! ./eval_target.sh "${DOCKER_FLAG[@]}" "${run_only[@]}" "$target" "${NUM_IT[@]}"; then
    echo "[WARN] $target failed"
    failed+=("$target")
  fi
done

echo "[INFO] Done. ${#failed[@]} failed${failed:+: ${failed[*]}}"
