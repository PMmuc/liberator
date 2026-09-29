#!/bin/bash -e

##
# Pre-requirements:
# - env TARGET: target name (from targets/)  [required unless BUILD_ONLY is set]
#
# Optional env:
# - BUILD_ONLY: build the image and exit (no container is run)
# - SKIP_BUILD: skip the image build, just run the container
# - CPUSET:     pin the container to these CPUs (e.g. "0" or "0,1")
##

# Builds the docker image for target libfuzzpp_analysis
# and runs it for the TARGET

IMG_NAME="libpp-analysis-new"
LIBPP="$(cd "$(dirname "$0")/.." && pwd)"

# --- Build phase (skipped when SKIP_BUILD is set) ---
if [ -z "${SKIP_BUILD:-}" ]; then
  source "$(dirname "$0")/llvm_source.sh"
  set -x
  # maybe add --no-cache \
  DOCKER_BUILDKIT=1 docker build \
    --build-arg USER_UID=$(id -u) --build-arg GROUP_UID=$(id -g) \
    --build-arg LLVM_SOURCE="$LLVM_SOURCE" \
    -t "$IMG_NAME" --target libfuzzpp_analysis_new \
    -f "$LIBPP/Dockerfile" "$LIBPP"
  set +x
fi

# Build-only mode: the image is ready, nothing to run. TARGET not required here.
if [ -n "${BUILD_ONLY:-}" ]; then
  echo "[INFO] Image build complete (BUILD_ONLY)."
  exit 0
fi

# --- Run phase (needs a TARGET) ---
TARGET="${TARGET:-${target:-}}"
if [ -z "${TARGET:-}" ]; then
  echo "[ERROR] \$TARGET must be specified as an environment variable"
  exit 1
fi

if [ -s "$LIBPP/analysis/$TARGET/work/apipass/conditions.json" ]; then
  echo "[INFO] Analysis for $TARGET already exists"
  exit 0
fi

# Pin to specific CPUs when requested by the orchestrator.
cpu_arg=()
[ -n "${CPUSET:-}" ] && cpu_arg=(--cpuset-cpus "$CPUSET")

if [[ "${DEVENV:-}" ]]; then
  docker run --env TARGET=${TARGET} -v "$LIBPP:/workspaces/libfuzz" \
    "$IMG_NAME"
else
  # Remove any leftover container with the same name
  docker rm -f "${IMG_NAME}-${TARGET}" >/dev/null 2>&1 || true
  docker run --rm --name "${IMG_NAME}-${TARGET}" "${cpu_arg[@]}" \
    --env TARGET=${TARGET} -v "$LIBPP:/workspaces/libfuzz" "$IMG_NAME"
fi
