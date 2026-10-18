#!/bin/bash
set -e

export LIBFUZZ=/workspaces/libfuzz/
export TARGET=$LIBFUZZ/analysis/openssl/

# RUN_ONLY=1 (or --run-only): skip fetching/building, reuse the previous build in $WORK
RUN_ONLY=${RUN_ONLY:-0}
if [ "$1" == "--run-only" ]; then
  RUN_ONLY=1
fi

WORK="$TARGET/work"

if [ "$RUN_ONLY" = "1" ]; then
  if [ ! -f "$WORK/lib/libssl.a" ]; then
    echo "[ERROR] --run-only: $WORK/lib/libssl.a not found, run a full build first"
    exit 1
  fi
  # Only clean the analysis results, keep the built library and headers
  rm -rf "$WORK/apipass"
else
  ./fetch.sh

  rm -rf "$WORK"
  mkdir -p "$WORK"
  mkdir -p "$WORK/lib" "$WORK/include"
fi

export CC=wllvm
export CXX=wllvm++
export LLVM_COMPILER=clang
export LLVM_COMPILER_PATH=$LLVM_DIR/bin
export LIBFUZZ_LOG_PATH=$WORK/apipass
CFLAGS="-g -O0"

mkdir -p "$LIBFUZZ_LOG_PATH"

# build the libpng library
cd "$TARGET/repo"

# CONFIGURE_FLAGS=""
# if [[ $CFLAGS = *sanitize=memory* ]]; then
#   CONFIGURE_FLAGS="no-asm"
# fi

# the config script supports env var LDLIBS instead of LIBS
export LDLIBS="$LIBS"

if [ "$RUN_ONLY" != "1" ]; then
  ./config --debug no-tests $CFLAGS -fno-sanitize=alignment $CONFIGURE_FLAGS --prefix="$WORK"
fi

# configure compiles some shits for testing, better remove it
rm -f $LIBFUZZ_LOG_PATH/apis_clang.json

touch $LIBFUZZ_LOG_PATH/exported_functions.txt
touch $LIBFUZZ_LOG_PATH/incomplete_types.txt
touch $LIBFUZZ_LOG_PATH/apis_clang.json
touch $LIBFUZZ_LOG_PATH/coerce.log

# CXXFLAGS="-fPIC -DOPENSSL_PIC"

if [ "$RUN_ONLY" != "1" ]; then
  make -j$(nproc) clean
  make -j$(nproc) LDCMD="$CXX $CXXFLAGS"
  make install
fi
# # make -j$(nproc)

extract-bc -b $WORK/lib/libssl.a

# this extracts the exported functions in a file, to be used later for grammar generations
$TOOLS_DIR/tool/misc/extract_included_functions.py -i "$WORK/include" \
  -p "$LIBFUZZ/targets/${TARGET_NAME}/public_headers.txt" \
  -e "$LIBFUZZ_LOG_PATH/exported_functions.txt" \
  -t "$LIBFUZZ_LOG_PATH/incomplete_types.txt" \
  -a "$LIBFUZZ_LOG_PATH/apis_clang.json" \
  -n "$LIBFUZZ_LOG_PATH/enum_types.txt" \
  -only_public_headers

# extract fields dependency from the library itself, repeat for each object produced
cd "$WORK"/apipass

/usr/bin/time -f "real %e\nuser %U\nsys %S\nmaxrss_kb %M" $PROF_EXTRACTOR $TOOLS_DIR/condition_extractor/bin/extractor \
  $WORK/lib/libssl.a.bc \
  -interface "$LIBFUZZ_LOG_PATH/apis_clang.json" \
  -output "$LIBFUZZ_LOG_PATH/conditions.json" \
  -minimize_api "$LIBFUZZ_LOG_PATH/apis_minimized.txt" \
  -v v0 -t json -do_indirect_jumps \
  -data_layout "$LIBFUZZ_LOG_PATH/data_layout.txt"
