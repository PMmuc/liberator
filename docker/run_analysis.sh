#!/bin/bash -e

##
# Pre-requirements:
# - env TARGET: target name (from targets/)
##

set -x

ROOT_DIR=$(dirname $(dirname $(realpath run_analysis.sh)))/
echo $ROOT_DIR

if [ -z "$TARGET" ]; then
  echo "[ERROR] \$TARGET must be specified as an environment variable"
  exit 1
fi

IMG_NAME="libpp-analysis"
LIBPP=../

if [ -s "$LIBPP/analysis/$TARGET/work/apipass/conditions.json" ]; then
  echo "[INFO] Analysis for $TARGET already exists"
  exit 0
fi

#set -x
DOCKER_BUILDKIT=1 docker build \
  --build-arg USER_UID=$(id -u) --build-arg GROUP_UID=$(id -g) \
  -t "$IMG_NAME" --target libfuzzpp_analysis \
  -f "$LIBPP/Dockerfile" "$LIBPP"
#set +x

echo "$IMG_NAME"

# Parse arguments
PROF_FLAG=""
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --prof) PROF_FLAG="-e PROF_EXTRACTOR='perf record -g --call-graph dwarf -F 99'"; shift ;;
        *) echo "Unknown parameter passed: $1"; exit 1 ;;
    esac
done

if [[ "${DEVENV}" ]]; then
  docker run ${PROF_FLAG} --env TARGET=${TARGET} -v "$(pwd)/..:/workspaces/libfuzz" \
    "$IMG_NAME"
else
  docker run ${PROF_FLAG} --rm -d --name "${IMG_NAME}-${TARGET}" \
    --env TARGET=${TARGET} -v "$ROOT_DIR:/workspaces/libfuzz" "$IMG_NAME"
fi
