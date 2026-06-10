#!/bin/bash
#
# Run the analysis for every target in Docker, keeping up to one container per
# CPU running at a time. Each worker is pinned to a distinct CPU (0,1,2,3 by
# default); as soon as one finishes, the next target is started on that CPU.
#
# Override the CPU set with:  CPUS_LIST="0 1 2 3 4 5" ./run_all_targets.sh
#
set -u

# Ensure we are in the root directory
if [ ! -f "docker/run_analysis.sh" ]; then
    echo "[ERROR] Please run this script from the root directory of the repository."
    exit 1
fi

read -ra CPUS <<< "${CPUS_LIST:-0 1 2 3}"
LOG_DIR="logs"
mkdir -p "$LOG_DIR"

# Collect targets that actually have an analysis.sh
targets=()
for target_dir in targets/*; do
    if [ -d "$target_dir" ] && [ -f "$target_dir/analysis.sh" ]; then
        targets+=("$(basename "$target_dir")")
    fi
done

n=${#targets[@]}
echo "[INFO] $n targets; up to ${#CPUS[@]} concurrent on CPUs: ${CPUS[*]}"

if [ "$n" -eq 0 ]; then
    echo "[INFO] No targets found."
    exit 0
fi

# Build the image once so the workers don't race on (or repeat) docker build.
echo "[INFO] Building docker image once..."
if ! ( cd docker && BUILD_ONLY=1 ./run_analysis.sh ); then
    echo "[ERROR] docker image build failed"
    exit 1
fi

# Per-CPU bookkeeping: which pid/target currently occupies each CPU ("" = free).
declare -A slot_pid slot_target
for cpu in "${CPUS[@]}"; do slot_pid[$cpu]=""; done

launch() {  # $1 = cpu, $2 = target
    local cpu="$1" target="$2"
    echo "[INFO] [cpu $cpu] start  $target  (log: $LOG_DIR/$target.log)"
    ( cd docker && CPUSET="$cpu" SKIP_BUILD=1 TARGET="$target" ./run_analysis.sh ) \
        >"$LOG_DIR/$target.log" 2>&1 &
    slot_pid[$cpu]=$!
    slot_target[$cpu]="$target"
}

reap() {  # $1 = pid that finished, $2 = its exit code
    local pid="$1" rc="$2" cpu target
    for cpu in "${CPUS[@]}"; do
        if [ "${slot_pid[$cpu]}" = "$pid" ]; then
            target="${slot_target[$cpu]}"
            # Judge success by the produced artifact, not the container exit code:
            # start_analysis.sh ends on an optional gprof/Excel step that exits 1
            # whenever gmon.out is absent (i.e. every non-profiling run), even though
            # the analysis itself already ran. The extractor writes
            # <target>_function_pointers.txt next to the -output file (work/apipass/).
            artifact="analysis/$target/work/apipass/${target}_function_pointers.txt"
            if [ -s "$artifact" ]; then
                echo "[SUCCESS] [cpu $cpu] $target"
            else
                echo "[ERROR]   [cpu $cpu] $target (exit $rc, missing $artifact — see $LOG_DIR/$target.log)"
            fi
            slot_pid[$cpu]=""
            return
        fi
    done
}

free_cpu() {  # prints a free CPU id, or nothing
    local cpu
    for cpu in "${CPUS[@]}"; do
        if [ -z "${slot_pid[$cpu]}" ]; then echo "$cpu"; return; fi
    done
}

i=0
running=0
while [ "$i" -lt "$n" ] || [ "$running" -gt 0 ]; do
    cpu="$(free_cpu)"
    if [ -n "$cpu" ] && [ "$i" -lt "$n" ]; then
        launch "$cpu" "${targets[$i]}"
        i=$((i + 1))
        running=$((running + 1))
        continue
    fi

    # No free CPU (or no work left to start): block until a worker exits.
    finished_pid=""
    wait -n -p finished_pid
    rc=$?
    [ -n "$finished_pid" ] && reap "$finished_pid" "$rc"
    running=$((running - 1))
done

echo "[INFO] All target analyses have finished."
