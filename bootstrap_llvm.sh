#!/bin/bash

# make sure to be in the right folder
echo "[INFO] Moving to ${LIBFUZZ}"
cd ${LIBFUZZ}

is_docker() {
  [ -f /.dockerenv ] || grep -q docker /proc/1/cgroup 2>/dev/null
}

is_ubuntu() {
  if [ -f /etc/os-release ]; then
    return true
  fi

  return false
}

if [ is_ubuntu ]; then
  # I need to install system clang
  echo "[INFO] Install clang-12"
  sudo ./update-alternatives-clang.sh 12 200
fi

if [ ! -d llvm-project ]; then
  echo "[INFO] llvm-project not present, download it"
  # download LLVM 14
  ./install_llvm.sh
else
  echo "[INFO] llvm-project already here! I assume it is OK"
fi

cp custom-libfuzzer/build.sh llvm-project/

# compile my LLVM 14
echo "[INFO] compile LLVM"
./llvm-project/build.sh

