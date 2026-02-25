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
cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..
