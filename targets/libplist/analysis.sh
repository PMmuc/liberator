#!/bin/bash
set -e
set -x

# NOTE: if TOOLD_DIR is unset, I assume to find stuffs in LIBFUZZ folder
if [ -z $TOOLS_DIR ]; then
    TOOLS_DIR=$LIBFUZZ
fi

WORK="$TARGET/work"
rm -rf "$WORK"
mkdir -p "$WORK"
mkdir -p "$WORK/lib" "$WORK/include"

export CC=wllvm
export CXX=wllvm++
export LLVM_COMPILER=clang
export LLVM_COMPILER_PATH=/usr/bin

export LIBFUZZ_LOG_PATH=$WORK/apipass

wllvm --version
mkdir -p $LIBFUZZ_LOG_PATH

echo "make 1"
cd "$TARGET/repo"
# libplist 1.3+ builds with cmake; force the static lib and a g++ host compiler
export CXX=g++
find . -name CMakeLists.txt -exec sed -i 's/SHARED//g' {} \;
rm -rf "$TARGET/repo/build"
mkdir -p "$TARGET/repo/build"
cd "$TARGET/repo/build"
cmake .. -DCMAKE_CXX_COMPILER=g++ -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
        -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS_DEBUG="-g -O0" \
        -DCMAKE_CXX_FLAGS_DEBUG="-g -O0" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5

# configure compiles some shits for testing, better remove it
rm $LIBFUZZ_LOG_PATH/apis.log || true

touch $LIBFUZZ_LOG_PATH/exported_functions.txt
touch $LIBFUZZ_LOG_PATH/incomplete_types.txt
touch $LIBFUZZ_LOG_PATH/apis_clang.json
touch $LIBFUZZ_LOG_PATH/apis_llvm.json
touch $LIBFUZZ_LOG_PATH/coerce.log

echo "make clean"
cd "$TARGET/repo/build"
make -j$(nproc) clean
echo "make"
make -j$(nproc)
echo "make install"
make install

extract-bc -b $WORK/lib/libplist.a

# this extracts the exported functions in a file, to be used later for grammar generations
$TOOLS_DIR/tool/misc/extract_included_functions.py -i "$WORK/include" \
    -p "$LIBFUZZ/targets/${TARGET_NAME}/public_headers.txt" \
    -e "$LIBFUZZ_LOG_PATH/exported_functions.txt" \
    -t "$LIBFUZZ_LOG_PATH/incomplete_types.txt" \
    -a "$LIBFUZZ_LOG_PATH/apis_clang.json" \
    -n "$LIBFUZZ_LOG_PATH/enum_types.txt"

# extract fields dependency from the library itself, repeat for each object produced
    cd "$WORK"/apipass

$PROF_EXTRACTOR $TOOLS_DIR/condition_extractor/bin/extractor \
    $WORK/lib/libplist.a.bc \
    -interface "$LIBFUZZ_LOG_PATH/apis_clang.json" \
    -output "$LIBFUZZ_LOG_PATH/conditions.json" \
    -minimize_api "$LIBFUZZ_LOG_PATH/apis_minimized.txt" \
    -v v0 -t json -do_indirect_jumps \
    -data_layout "$LIBFUZZ_LOG_PATH/data_layout.txt" \
    -target_name "$TARGET_NAME"
