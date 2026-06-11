#!/bin/bash

# Centralized Analysis Script
# Usage: ./analysis.sh [target_name]
# If target_name is not provided, it can be iterated from targets.txt (not implemented in this simplified version, better to run per target)

set -e
set -x

echo "Analysing $TARGET_NAME"

source setup_target.sh $1

export TARGET="$LIBFUZZ/analysis/$TARGET_NAME/"
export WORK="$TARGET/work"

FETCH_ONLY=false
RUN_ONLY=false
DEBUG_ONLY=false
BUILD_ONLY=false

if [ "$2" == "--fetch-only" ]; then
  FETCH_ONLY=true
elif [ "$2" == "--run-only" ]; then
  RUN_ONLY=true
elif [ "$2" == "--debug-only" ]; then
  DEBUG_ONLY=true
elif [ "$2" == "--build-only" ]; then
  BUILD_ONLY=true
fi

# Check if running in Docker
is_docker() {
  [ -f /.dockerenv ] || grep -q docker /proc/1/cgroup 2>/dev/null
}

# Load target configuration
# Pull target library into $WORK directory
# This must provide:
# - REPO_URL - the url of the target library.
# - REPO_COMMIT - the commit tag to be pulled.
# - target_configure() function
# - BC_FILE_NAME (name of the bitcode/archive to extract, e.g., libxml2.a)
# Optional:
# - target_build() function (defaults to make)
# - target_preinstall() function
source "$CONFIG_FILE"

# --- Fetch Phase ---
if [ "$RUN_ONLY" = false ] && [ "$DEBUG_ONLY" = false ]; then
  # Clean work directory for a fresh build
  rm -rf "$WORK"
  mkdir -p "$WORK"

  if [ ! -d "$TARGET/repo" ]; then
    git clone --no-checkout "$REPO_URL" "$TARGET/repo"
  fi

  # Always checkout the correct commit, even if repo exists
  git -C "$TARGET/repo" checkout "$REPO_COMMIT"
fi

if [ "$FETCH_ONLY" = true ]; then
  echo "Fetch complete. Exiting."
  exit 0
fi

# Check if wllvm is installed
if ! command -v wllvm &>/dev/null; then
  echo "[ERROR] wllvm is not installed or not in PATH. Please install it (e.g., pip install wllvm)."
  exit 1
fi

if [ "$RUN_ONLY" = false ] && [ "$DEBUG_ONLY" = false ]; then
  # Go to repo
  cd "$TARGET/repo"

  # Pre-install hook
  if is_docker && type target_preinstall_docker &>/dev/null; then
    echo "[INFO] Detected Docker environment. Running docker preinstall steps..."
    target_preinstall_docker
  fi

  if type target_preinstall &>/dev/null; then
    echo "[INFO] Running preinstall steps..."
    target_preinstall
  fi

  # --- Fetch Phase ---

  # Configure
  target_configure

  # Clean (optional but good practice)
  if [ -f Makefile ]; then
    make clean || true
  fi

  # Build
  if type target_build &>/dev/null; then
    target_build
  else
    make -j$(nproc)
  fi

  # Install (usually needed to populate WORK)
  # Provide a default if not overridden? Most scripts did 'make install'
  # We'll assume the user might want to override install step too, but usually it's `make install`
  # If target_build didn't install, we might need an explicit install step.
  # For now, let's assume `target_build` does everything or we add `target_install`
  if type target_install &>/dev/null; then
    target_install
  else
    make install
  fi
fi

cd $PROJECT
if [ "$BUILD_ONLY" = false ]; then
  if [ "$DEBUG_ONLY" = true ]; then
    source run_analysis.sh --debug
  else
    source run_analysis.sh
  fi
fi
echo "Analysis complete for $TARGET_NAME"
