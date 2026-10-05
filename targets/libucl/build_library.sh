#!/bin/bash
set -e

# export TARGET=/tmp/libtiff

if [ ! -d "$TARGET/repo" ]; then
    echo "fetch.sh must be executed first."
    exit 1
fi

export CC=$LLVM_DIR/bin/clang
export CXX=$LLVM_DIR/bin/clang++

WORK="$TARGET/work"
rm -rf "$WORK"
mkdir -p "$WORK"
mkdir -p "$WORK/lib" "$WORK/include"

echo "make 1"
cd "$TARGET/repo"

# Compile library for coverage
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping -g" \
        -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping -g"

echo "make clean"
make -j$(nproc) clean
echo "make"
make -j$(nproc)
echo "make install"
make install

mv $WORK/lib/libucl.a $WORK/lib/libucl_profile.a

# Compile library for debugging
cd "$TARGET/repo"
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address -g" \
        -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address -g"

echo "make clean"
make -j$(nproc) clean
echo "make"
make -j$(nproc)
echo "make install"
make install

mv $WORK/lib/libucl.a $WORK/lib/libucl_cluster.a

# Compile library for fuzzing
cd "$TARGET/repo"
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address" \
        -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address"

echo "make"
make -j$(nproc)
echo "make install"
make install

echo "[INFO] Library installed in: $WORK"
