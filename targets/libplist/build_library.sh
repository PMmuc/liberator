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
# libplist 1.3+ builds with cmake; force the static lib and a g++ host compiler
export CXX=g++
find . -name CMakeLists.txt -exec sed -i 's/SHARED//g' {} \;

# Compile library for coverage
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_CXX_COMPILER=g++ -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
        -DENABLE_STATIC=on \
        -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping -g" \
        -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping -g" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5

echo "make clean"
make -j$(nproc) clean
echo "make"
make -j$(nproc)
echo "make install"
make install

mv $WORK/lib/libplist-2.0.a $WORK/lib/libplist-2.0_profile.a

# Compile library for debugging
cd "$TARGET/repo"
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_CXX_COMPILER=g++ -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
        -DENABLE_STATIC=on \
        -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address -g" \
        -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address -g" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5

echo "make clean"
make -j$(nproc) clean
echo "make"
make -j$(nproc)
echo "make install"
make install

mv $WORK/lib/libplist-2.0.a $WORK/lib/libplist-2.0_cluster.a

# Compile library for fuzzing
cd "$TARGET/repo"
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_CXX_COMPILER=g++ -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
        -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address" \
        -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5

echo "make clean"
make -j$(nproc) clean
echo "make"
make -j$(nproc)
echo "make install"
make install

echo "[INFO] Library installed in: $WORK"
