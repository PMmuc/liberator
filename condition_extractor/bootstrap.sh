#!/bin/bash
# Building condition extractor for docker
set -x
set -e

. ./env.sh
export PATH=$LLVM_DIR/bin:$PATH
export CXXFLAGS="-Wno-deprecated-declarations -Wfatal-errors"

if [[ -d "$TOOLS_DIR/condition_extractor/build_release" ]]; then
  rm -rf $TOOLS_DIR/condition_extractor/build_release
fi

mkdir -p $TOOLS_DIR/condition_extractor/build_release
cd $TOOLS_DIR/condition_extractor/build_release
CC=clang CXX=clang++ cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_TESTS=OFF \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld" \
  -DCMAKE_MODULE_LINKER_FLAGS="-fuse-ld=lld" ..
