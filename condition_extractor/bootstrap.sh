#!/bin/bash
# Building condition extractor for docker
set -x
set -e

. ./env.sh
# cmake .
export PATH=$LLVM_DIR/bin:$PATH
export CXXFLAGS="-Wno-deprecated-declarations -Wfatal-errors"
# cmake -DCMAKE_BUILD_TYPE=Release .
mkdir -p $TOOLS_DIR/condition_extractor/build
cd $TOOLS_DIR/condition_extractor/build
CC=clang CXX=clang++ cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld" \
  -DCMAKE_MODULE_LINKER_FLAGS="-fuse-ld=lld" ..
