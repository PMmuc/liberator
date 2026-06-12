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
LIBPP=../

# --- Build phase (skipped when SKIP_BUILD is set) ---
if [ -z "${SKIP_BUILD:-}" ]; then
  set -x
  DOCKER_BUILDKIT=1 docker build \
    --build-arg USER_UID=$(id -u) --build-arg GROUP_UID=$(id -g) \
    --build-arg USE_LOCAL_LLVM="false" \
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
  docker run --env TARGET=${TARGET} -v "$(pwd)/..:/workspaces/libfuzz" \
    "$IMG_NAME"
else
  # Remove any leftover container with the same name (e.g. a crashed/killed run
  # where --rm never fired) so the name can be reused.
  docker rm -f "${IMG_NAME}-${TARGET}" >/dev/null 2>&1 || true
  # Run in the FOREGROUND (no -d): this streams the analysis output to stdout so
  # the orchestrator can capture it in logs/<target>.log, and blocks until the
  # analysis finishes so per-CPU throttling and the artifact check work.
  docker run --rm --name "${IMG_NAME}-${TARGET}" "${cpu_arg[@]}" \
    --env TARGET=${TARGET} -v "$(pwd)/..:/workspaces/libfuzz" "$IMG_NAME"
fi
