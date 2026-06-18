#!/bin/bash -e

# Builds LLVM from source inside Docker and extracts it to the host's llvm-21 folder.
# This replaces the broken or missing ./llvm-21 folder on the host.

LIBPP="$(cd "$(dirname "$0")/.." && pwd)"
EXTRACT_DIR="$LIBPP/llvm-21-extract"

echo "[INFO] Building LLVM from source (this will take a while)..."

# Build only the llvm_source_build target and output it to a local directory
DOCKER_BUILDKIT=1 docker build \
  --no-cache \
  --target llvm_source_build \
  --output type=local,dest="$EXTRACT_DIR" \
  -f "$LIBPP/Dockerfile" "$LIBPP"

echo "[INFO] Build complete. Moving extracted LLVM to ./llvm-21..."

# Remove old llvm-21 if it exists
if [ -d "$LIBPP/llvm-21" ]; then
  rm -rf "$LIBPP/llvm-21"
fi

# The output folder contains an 'llvm' directory from the root of the scratch stage
mv "$EXTRACT_DIR/llvm" "$LIBPP/llvm-21"
rm -rf "$EXTRACT_DIR"

echo "[INFO] Successfully copied LLVM build to $LIBPP/llvm-21"
