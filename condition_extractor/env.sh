#!/bin/bash

# Setting up Environment variables for buliding condition extractor.

# PROJECTHOME=$(pwd)
# sysOS=`uname -s`
# LLVMHome="llvm-14.0.0.obj"
# Z3Home="z3.obj"
# SVF_install_path=`npm root`
# export LLVM_DIR=$SVF_install_path/$LLVMHome
# export Z3_DIR=$SVF_install_path/$Z3Home
# export PATH=$LLVM_DIR/bin:$PATH
# export PATH=$PROJECTHOME/bin:$PATH
# echo "export LLVM_DIR=$SVF_install_path/$LLVMHome" >> ~/.bashrc
# echo "export Z3_DIR=$SVF_install_path/$Z3Home" >> ~/.bashrc
# echo "export PATH=$LLVM_DIR/bin:$PROJECTHOME/bin:$PATH" >> ~/.bashrc
# if [[ $sysOS == "Darwin" ]]
# then
#     export SVF_DIR=$SVF_install_path/SVF/
# elif [[ $sysOS == "Linux" ]]
# then
#     export SVF_DIR=$SVF_install_path/SVF/
# fi

# echo "LLVM_DIR="$LLVM_DIR
# echo "SVF_DIR="$SVF_DIR
# echo "Z3_DIR="$Z3_DIR

PROJECTHOME=$(pwd)
sysOS=$(uname -s)
SVF_install_path=/home/libfuzz/SVF-${SVF_VERSION}
SVF_cmake_path=$SVF_install_path/lib/cmake/SVF
LLVM_install_path=/home/libfuzz/llvm-16
LLVM_cmake_path=$LLVM_install_path/lib/cmake/llvm

if [ -z "${LLVM_DIR}"]; then
  export LLVM_DIR=$LLVM_cmake_path
  export PATH=$LLVM_DIR/bin:$PATH
  echo "export LLVM_DIR=$LLVM_cmake_path/" >>~/.bashrc
fi

if [ -z "${SVF_DIR}"]; then
  if [[ $sysOS == "Darwin" ]]; then
    export SVF_DIR=$SVF_cmake_path/
  elif [[ $sysOS == "Linux" ]]; then
    export SVF_DIR=$SVF_cmake_path/
  fi
fi

export PATH=$PROJECTHOME/bin:$PATH

#echo "export Z3_DIR=$SVF_install_path/$Z3Home" >>~/.bashrc
echo "export PATH=$LLVM_DIR/bin:$PROJECTHOME/bin:$PATH" >>~/.bashrc

echo "LLVM_DIR="$LLVM_DIR
echo "SVF_DIR="$SVF_DIR
echo "Z3_DIR="$Z3_DIR
