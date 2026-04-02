#!/bin/bash
# --- Analysis Phase ---
# 1. Clears work directory
# 2. Calls WLLVM to generate bitcode file
# 3. Calls extract_included_functions.py
# 4. Calls condition_extractor
# WORK is exported by analysis.sh or setup_target.sh
# export WORK="$TARGET/work"

DEBUG=""
if [ "$1" == "--debug" ]; then
  DEBUG="gdb --args "
fi

# Do not clean WORK here, as it might contain build artifacts (libs, includes)
# rm -rf "$WORK"
mkdir -p "$WORK"
mkdir -p "$WORK/lib" "$WORK/include"

export LIBFUZZ_LOG_PATH=$WORK/apipass
# Clean analysis logs only
rm -rf "$LIBFUZZ_LOG_PATH"
mkdir -p "$LIBFUZZ_LOG_PATH"

# Setup logs
touch "$LIBFUZZ_LOG_PATH/exported_functions.txt"
touch "$LIBFUZZ_LOG_PATH/incomplete_types.txt"
touch "$LIBFUZZ_LOG_PATH/apis_clang.json"
touch "$LIBFUZZ_LOG_PATH/apis_llvm.json"
touch "$LIBFUZZ_LOG_PATH/coerce.log"
touch "$LIBFUZZ_LOG_PATH/enum_types.txt"

if python3 -c "import wllvm" 2>/dev/null; then
  echo "[INFO] WLLVM is installed."
else
  echo "[ERROR] WLLVM package not found use 'pip install wllvm'."
  return 1
fi

# Sync condition_extractor if needed
if [ -d "$PROJECT/condition_extractor" ]; then
  echo "[INFO] Syncing condition_extractor..."
  rsync -au --exclude=".git" "$PROJECT/condition_extractor/" "$TOOLS_DIR/condition_extractor/"
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
  -t "$TARGET_NAME" \
  -p "$PUBLIC_HEADERS" \
  -e "$LIBFUZZ_LOG_PATH/exported_functions.txt" \
  -I "$LIBFUZZ_LOG_PATH/incomplete_types.txt" \
  -a "$LIBFUZZ_LOG_PATH/apis_clang.json" \
  -n "$LIBFUZZ_LOG_PATH/enum_types.txt"

# Extractor
# changing the working directory to $WORK will cause the gmon.out file to be stored in
# the $WORK directory
cd "$WORK"
time $DEBUG "$TOOLS_DIR/condition_extractor/build/bin/extractor" \
  "${ARCHIVE_PATH}.bc" \
  -interface "$LIBFUZZ_LOG_PATH/apis_clang.json" \
  -output "$LIBFUZZ_LOG_PATH/conditions.json" \
  -minimize_api "$LIBFUZZ_LOG_PATH/apis_minimized.txt" \
  -v v0 -t json -do_indirect_jumps \
  -data_layout "$LIBFUZZ_LOG_PATH/data_layout.txt" \
  ${EXTRA_EXTRACTOR_FLAGS}

echo "Saved to ${LIBFUZZ_LOG_PATH}/conditions.json"

# Post-analysis hook (optional)
if type target_post_analysis &>/dev/null; then
  target_post_analysis
fi
