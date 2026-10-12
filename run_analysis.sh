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

if [ -n "$FUNCTION" ]; then
  export LIBFUZZ_LOG_PATH=$WORK/apipass_debug/$FUNCTION
  EXTRACTOR_BUILD_DIR=${EXTRACTOR_BUILD_DIR:-build}
  LOG_TAGS=${LOG_TAGS:-Summary,GEPHandler,handler,APARAM, paramMetadata,returnMetadata}
else
  export LIBFUZZ_LOG_PATH=$WORK/apipass
fi

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
  rsync -au --exclude=".git" --exclude="build" --exclude="build_release" "$PROJECT/condition_extractor/" "$TOOLS_DIR/condition_extractor/"
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

PUBLIC_HEADERS="$LIBFUZZ/targets/$TARGET_NAME/public_headers.txt"
if [ ! -s "$PUBLIC_HEADERS" ]; then
  echo "Error: public_headers.txt is missing"
  exit 1
fi

INCLUDE_DIR=${TARGET_INCLUDE_DIR:-"$WORK/include"}

"$PROJECT/tool/misc/extract_included_functions.py" -i "$INCLUDE_DIR" \
  -t "$TARGET_NAME" \
  -p "$PUBLIC_HEADERS" \
  -e "$LIBFUZZ_LOG_PATH/exported_functions.txt" \
  -I "$LIBFUZZ_LOG_PATH/incomplete_types.txt" \
  -a "$LIBFUZZ_LOG_PATH/apis_clang.json" \
  -n "$LIBFUZZ_LOG_PATH/enum_types.txt"

if [ -n "$FUNCTION" ]; then
  API_LIST="$LIBFUZZ_LOG_PATH/apis_clang.json"
  jq -c --arg f "$FUNCTION" 'select(.function_name == $f)' "$API_LIST" >"$API_LIST.tmp"
  if [ ! -s "$API_LIST.tmp" ]; then
    echo "[ERROR] $FUNCTION is not in $API_LIST"
    exit 1
  fi
  mv "$API_LIST.tmp" "$API_LIST"
fi

EXTRACTOR_BUILD_DIR=${EXTRACTOR_BUILD_DIR:-build_release}
EXTRACTOR_BIN="$TOOLS_DIR/condition_extractor/$EXTRACTOR_BUILD_DIR/bin/extractor"

if [ ! -x "$EXTRACTOR_BIN" ]; then
  echo "[ERROR] Extractor not found at $EXTRACTOR_BIN"
  echo "        Build it, or set EXTRACTOR_BUILD_DIR to another build directory."
  exit 1
fi
echo "[INFO] Using extractor: $EXTRACTOR_BIN"

if [ ! -x /usr/bin/time ]; then
  echo "[ERROR] /usr/bin/time not found"
  exit 1
fi

# -do_indirect_jumps for indirect calls
cd "$WORK"
/usr/bin/time -f "real %e\nuser %U\nsys %S\nmaxrss_kb %M" $DEBUG "$EXTRACTOR_BIN" \
  "${ARCHIVE_PATH}.bc" \
  -target "$TARGET_NAME" \
  -interface "$LIBFUZZ_LOG_PATH/apis_clang.json" \
  -output "$LIBFUZZ_LOG_PATH/conditions.json" \
  -minimize_api "$LIBFUZZ_LOG_PATH/apis_minimized.txt" \
  -v v0 -t json \
  -do_indirect_jumps \
  -data_layout "$LIBFUZZ_LOG_PATH/data_layout.txt" \
  -metrics \
  -log Summary \
  -profiling \
  \
  ${EXTRA_EXTRACTOR_FLAGS} #-range 81-334 \

echo "Saved to ${LIBFUZZ_LOG_PATH}/conditions.json"

# Post-analysis hook (optional)
if type target_post_analysis &>/dev/null; then
  target_post_analysis
fi
