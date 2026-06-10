#!/bin/bash -e

##
# Pre-requirements:
# - env TARGET: target name (from targets/)
##

set -x

ROOT_DIR=$(dirname $(dirname $(realpath run_analysis.sh)))/
echo $ROOT_DIR

IMG_NAME="libpp-analysis-org"
LIBPP=../

# Build the analysis image. Set SKIP_BUILD=1 to reuse an already-built image
# (e.g. when several workers run concurrently and the build was done once up front).
if [ "${SKIP_BUILD:-0}" != "1" ]; then
  DOCKER_BUILDKIT=1 docker build \
    --build-arg USER_UID=$(id -u) --build-arg GROUP_UID=$(id -g) \
    -t "$IMG_NAME" --target libfuzzpp_analysis_org \
    -f "$LIBPP/Dockerfile" "$LIBPP"
fi

# BUILD_ONLY=1: just (re)build the image and exit (no TARGET required).
if [ "${BUILD_ONLY:-0}" = "1" ]; then
  exit 0
fi

if [ -z "${TARGET:-}" ]; then
  echo "[ERROR] \$TARGET must be specified as an environment variable"
  exit 1
fi

if [ -s "$LIBPP/analysis/$TARGET/work/apipass/conditions.json" ]; then
  echo "[INFO] Analysis for $TARGET already exists"
  exit 0
fi

echo "$IMG_NAME"

# Parse arguments
PROF_FLAG=()
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --prof) PROF_FLAG=("PROF_EXTRACTOR=perf record -g --call-graph dwarf -F 99"); shift ;;
        *) echo "Unknown parameter passed: $1"; exit 1 ;;
    esac
done

# CPU(s) to pin this container to (default 0). The batch runner sets this per worker.
CPUSET="${CPUSET:-0}"

if [[ "${DEVENV}" ]]; then
  docker run --env PROF_FLAG=${PROF_FLAG} --env TARGET=${TARGET} -v "$(pwd)/..:/workspaces/libfuzz" \
    "$IMG_NAME"
else
  # Drop any stale container of the same name, then run pinned to $CPUSET in the
  # foreground so the caller can wait for completion and capture the real exit code.
  docker rm -f "${IMG_NAME}-${TARGET}" >/dev/null 2>&1 || true
  docker run --env PROF_FLAG=${PROF_FLAG} --rm --cpuset-cpus="$CPUSET" --name "${IMG_NAME}-${TARGET}" --user root \
    --env TARGET=${TARGET} -v "$ROOT_DIR:/workspaces/libfuzz" "$IMG_NAME"
fi
