#!/bin/bash
# Decide which LLVM source the Docker build should use and export LLVM_SOURCE.
#
#   * llvm_source_local     -> reuse the prebuilt ./llvm-21 directory (fast).
#   * llvm_source_download  -> download the precompiled LLVM 21 release (fast).
#   * llvm_source_build     -> build LLVM 21 from source in Docker (slow).
#
# Source this from a docker/run_*.sh script and pass the result on with
#   --build-arg LLVM_SOURCE="$LLVM_SOURCE"
#
# Auto-selection: local prebuilt ./llvm-21 when present, otherwise the
# precompiled download. Expects LIBPP to point at the repo root (the docker
# build context). An explicitly preset LLVM_SOURCE is respected, so you can
# force any source, e.g.:
#   LLVM_SOURCE=llvm_source_build    ./run_analysis.sh
#   LLVM_SOURCE=llvm_source_download ./run_analysis.sh

_repo="${LIBPP:-..}"

if [ -z "${LLVM_SOURCE:-}" ]; then
    if [ -d "${_repo}/llvm-21" ]; then
        LLVM_SOURCE="llvm_source_local"
        echo "[INFO] Found ${_repo}/llvm-21 -> using local prebuilt LLVM"
    else
        LLVM_SOURCE="llvm_source_download"
        echo "[INFO] No local llvm-21 -> downloading precompiled LLVM 21"
    fi
else
    echo "[INFO] LLVM_SOURCE preset to '${LLVM_SOURCE}'"
fi

export LLVM_SOURCE
