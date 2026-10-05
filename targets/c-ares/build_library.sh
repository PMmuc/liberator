#!/bin/bash

set -e
set -x

# NOTE: if TOOLD_DIR is unset, I assume to find stuffs in LIBFUZZ folder
if [ -z "$TOOLS_DIR" ]; then
    TOOLS_DIR=$LIBFUZZ
fi

WORK="$TARGET/work"
rm -rf "$WORK"
mkdir -p "$WORK"
mkdir -p "$WORK/lib" "$WORK/include"

export LLVM_COMPILER_PATH=$LLVM_DIR/bin
export CC="$LLVM_COMPILER_PATH"/clang
export CXX="$LLVM_COMPILER_PATH"/clang++

echo "make 1"
cd "$TARGET/repo"

# Compile library for coverage
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
        -DENABLE_STATIC=on -DCARES_STATIC=on \
        -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping -g" \
        -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping -g"

echo "make clean"
make -j"$(nproc)" clean
echo "make"
make -j"$(nproc)"
echo "make install"
make install

mv "$WORK"/lib/libcares.a "$WORK"/lib/libcares_profile.a

# Compile library for clustering
cd "$TARGET/repo"
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
        -DENABLE_STATIC=on -DCARES_STATIC=on \
        -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address -g" \
        -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address -g"

echo "make clean"
make -j"$(nproc)" clean
echo "make"
make -j"$(nproc)"
echo "make install"
make install

mv "$WORK"/lib/libcares.a "$WORK"/lib/libcares_cluster.a

# Compile library for fuzzing
cd "$TARGET/repo"
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
        -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Release -DCARES_STATIC=on \
        -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address" \
        -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address"

echo "make clean"
make -j"$(nproc)" clean
echo "make"
make -j"$(nproc)"
echo "make install"
make install
# configure compiles some shits for testing, better remove it
echo "[INFO] Library installed in: $WORK"
