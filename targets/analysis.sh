#!/bin/bash

# Centralized Analysis Script
# Usage: ./analysis.sh [target_name]
# If target_name is not provided, it can be iterated from targets.txt (not implemented in this simplified version, better to run per target)

set -e
set -x

# Function to print usage
usage() {
  echo "Usage: $0 <target_name>"
  echo "Available targets are listed in targets.txt"
  exit 1
}

if [ -z "$1" && -z "$TARGET"]; then
  usage
fi

if [ -d "$TARGET"]; then
  TARGET_NAME=$TARGET
else
  TARGET_NAME=$1
fi

cp -r $PROJECT/targets/${TARGET} $TOOLS_DIR/targets
cd ${LIBFUZZ}/targets/${TARGET}

FETCH_ONLY=false
if [ "$2" == "--fetch-only" ]; then
  FETCH_ONLY=true
fi

SCRIPT_DIR="$PROJECT"
TARGET_DIR="$SCRIPT_DIR/$TARGET_NAME"
CONFIG_FILE="$TARGET_DIR/config.sh"

if [ ! -d "$TARGET_DIR" ]; then
  echo "Error: Target directory $TARGET_DIR does not exist."
  exit 1
fi

if [ ! -f "$CONFIG_FILE" ]; then
  echo "Error: Config file $CONFIG_FILE not found."
  exit 1
fi

# Load common environment variables
# NOTE: if TOOLS_DIR is unset, I assume to find stuffs in LIBFUZZ folder
# This mimics the behavior of original scripts
if [ -z "$TOOLS_DIR" ]; then
  if [ -n "$LIBFUZZ" ]; then
    TOOLS_DIR=$LIBFUZZ
  else
    echo "Error: TOOLS_DIR or LIBFUZZ env variable must be set."
    exit 1
  fi
fi

# Default environment setup
export TARGET=$TARGET_DIR
export WORK="$TARGET/work"
export LLVM_COMPILER=clang
# Default LLVM_COMPILER_PATH if LLVM_DIR is set, otherwise rely on system path or config override
if [ -n "$LLVM_DIR" ]; then
  export LLVM_COMPILER_PATH=$LLVM_DIR/bin
fi
export CC=wllvm
export CXX=wllvm++

# Load target configuration
# This must provide:
# - REPO_URL
# - REPO_COMMIT
# - target_configure() function
# - BC_FILE_NAME (name of the bitcode/archive to extract, e.g., libxml2.a)
# Optional:
# - target_build() function (defaults to make)
# - target_preinstall() function
source "$CONFIG_FILE"

# --- Fetch Phase ---
if [ ! -d "$TARGET/repo" ]; then
  git clone --no-checkout "$REPO_URL" "$TARGET/repo"
fi

# Always checkout the correct commit, even if repo exists
git -C "$TARGET/repo" checkout "$REPO_COMMIT"

if [ "$FETCH_ONLY" = true ]; then
  echo "Fetch complete. Exiting."
  exit 0
fi

# --- Analysis Phase ---
rm -rf "$WORK"
mkdir -p "$WORK"
mkdir -p "$WORK/lib" "$WORK/include"

export LIBFUZZ_LOG_PATH=$WORK/apipass
mkdir -p "$LIBFUZZ_LOG_PATH"

# Check if running in Docker
is_docker() {
  [ -f /.dockerenv ] || grep -q docker /proc/1/cgroup 2>/dev/null
}

# Setup logs
touch "$LIBFUZZ_LOG_PATH/exported_functions.txt"
touch "$LIBFUZZ_LOG_PATH/incomplete_types.txt"
touch "$LIBFUZZ_LOG_PATH/apis_clang.json"
touch "$LIBFUZZ_LOG_PATH/apis_llvm.json"
touch "$LIBFUZZ_LOG_PATH/coerce.log"
touch "$LIBFUZZ_LOG_PATH/enum_types.txt"

# Go to repo
cd "$TARGET/repo"

# Pre-install hook (optional)
if type target_preinstall &>/dev/null; then
  if is_docker; then
    echo "[INFO] Detected Docker environment. Running preinstall steps..."
    target_preinstall
  elif command -v apt-get &>/dev/null; then
    echo "[INFO] Docker not detected, but 'apt-get' found. Running preinstall steps..."
    target_preinstall
  else
    echo "[WARN] Skipping preinstall steps (Not in Docker and 'apt-get' not found)."
    echo "[WARN] Please ensure dependencies are installed manually."
  fi
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

# --- Extraction Phase ---
# Check if BC_FILE_NAME is provided
if [ -z "$BC_FILE_NAME" ]; then
  echo "Error: BC_FILE_NAME not defined in config.sh"
  exit 1
fi

ARCHIVE_PATH="$WORK/lib/$BC_FILE_NAME"
if [ ! -f "$ARCHIVE_PATH" ]; then
  echo "Error: Archive $ARCHIVE_PATH not found."
  ls -R "$WORK/lib"
  exit 1
fi

# Let WLLVM generate bitcode file
extract-bc -b "$ARCHIVE_PATH"

# Extract included functions
# Use public_headers.txt from target dir if it exists
PUBLIC_HEADERS="$TARGET/public_headers.txt"
if [ ! -f "$PUBLIC_HEADERS" ]; then
  echo "Warning: public_headers.txt not found at $PUBLIC_HEADERS"
fi

# Extract included functions
# Use public_headers.txt from target dir if it exists
PUBLIC_HEADERS="$TARGET/public_headers.txt"
if [ ! -f "$PUBLIC_HEADERS" ]; then
  echo "Warning: public_headers.txt not found at $PUBLIC_HEADERS"
fi

INCLUDE_DIR=${TARGET_INCLUDE_DIR:-"$WORK/include"}

"$TOOLS_DIR/tool/misc/extract_included_functions.py" -i "$INCLUDE_DIR" \
  -p "$PUBLIC_HEADERS" \
  -e "$LIBFUZZ_LOG_PATH/exported_functions.txt" \
  -t "$LIBFUZZ_LOG_PATH/incomplete_types.txt" \
  -a "$LIBFUZZ_LOG_PATH/apis_clang.json" \
  -n "$LIBFUZZ_LOG_PATH/enum_types.txt"

# Extractor
"$TOOLS_DIR/condition_extractor/bin/extractor" \
  "${ARCHIVE_PATH}.bc" \
  -interface "$LIBFUZZ_LOG_PATH/apis_clang.json" \
  -output "$LIBFUZZ_LOG_PATH/conditions.json" \
  -minimize_api "$LIBFUZZ_LOG_PATH/apis_minimized.txt" \
  -v v0 -t json -do_indirect_jumps \
  -data_layout "$LIBFUZZ_LOG_PATH/data_layout.txt" \
  ${EXTRA_EXTRACTOR_FLAGS}

# Post-analysis hook (optional)
if type target_post_analysis &>/dev/null; then
  target_post_analysis
fi

echo "Analysis complete for $TARGET_NAME"
