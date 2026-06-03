#!/bin/bash

# docker build -t vsc-libfuzz-230af0437939dbfc00081fc163ef918b -f Dockerfile .
# docker run -it -v "$(pwd):/workspaces/libfuzz" vsc-libfuzz-230af0437939dbfc00081fc163ef918b zsh

if [ $# -ne 1 ]; then
  echo "[ERROR] Specify which target you want to debug: $0 TARGET_NAME"
  exit 1
fi
TARGET=$1

echo "Debugging target ${TARGET}"

DOCKER_IMAGE=libfuzzpp_debug_${TARGET}_new

# Build the libfuzzpp_debug stage of docker.
# This stage copies a locally build or downloads a prebuilt LLVM version.
# Then it builds SVF 3.3.
DOCKER_BUILDKIT=1 docker build --build-arg USER_UID=$(id -u) \
  --build-arg GROUP_UID=$(id -g) --target libfuzzpp_debug_new \
  --build-arg target_name="${TARGET}" \
  --build-arg USE_LOCAL_LLVM="true" \
  -t ${DOCKER_IMAGE} -f Dockerfile .
docker run --name libfuzz_debug_${TARGET}_new -it -v $(pwd):/workspaces/libfuzz ${DOCKER_IMAGE} zsh
