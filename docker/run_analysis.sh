#!/bin/bash -e

##
# Pre-requirements:
# - env TARGET: target name (from targets/)
##

# Builds the docker image for target libfuzzpp_analysis
# and runs it for the TARGET

if [ -z "$TARGET" ]; then
  echo "[ERROR] \$TARGET must be specified as an environment variable"
  exit 1
fi

IMG_NAME="libpp-analysis-new"
LIBPP=../

if [ -s "$LIBPP/analysis/$TARGET/work/apipass/conditions.json" ]; then
  echo "[INFO] Analysis for $TARGET already exists"
  exit 0
fi

set -x
DOCKER_BUILDKIT=1 docker build \
  --build-arg USER_UID=$(id -u) --build-arg GROUP_UID=$(id -g) \
  --build-arg USE_LOCAL_LLVM="false" \
  -t "$IMG_NAME" --target libfuzzpp_analysis_new \
  -f "$LIBPP/Dockerfile" "$LIBPP"
set +x

if [[ "${DEVENV}" ]]; then
  docker run --env TARGET=${TARGET} -v "$(pwd)/..:/workspaces/libfuzz" \
    "$IMG_NAME"
else
  # Remove any leftover container with the same name (e.g. a crashed/killed run
  # where --rm never fired) so the name can be reused.
  docker rm -f "${IMG_NAME}-${TARGET}" >/dev/null 2>&1 || true
  docker run --rm -d --name "${IMG_NAME}-${TARGET}" \
    --env TARGET=${TARGET} -v "$(pwd)/..:/workspaces/libfuzz" "$IMG_NAME"
fi
