# Function to print usage
usage() {
  echo "Usage: $0 <target_name>"
  echo "Available targets are listed in targets.txt"
  exit 1
}

source setup.sh

if [[ ! -z "$1" ]]; then
  export TARGET_NAME="$1"
elif [ ! -z "$TARGET" ]; then
  export TARGET_NAME=$TARGET
fi

export GEN_TARGET_DIR=$LIBFUZZ/targets
export TARGET_DIR=$GEN_TARGET_DIR/$TARGET_NAME

CONFIG_FILE="$TARGET_DIR/config.sh"

# Default environment setup
export LLVM_COMPILER=clang
# Default LLVM_COMPILER_PATH if LLVM_DIR is set, otherwise rely on system path or config override
if [ -n "$LLVM_DIR" ]; then
  export LLVM_COMPILER_PATH=$LLVM_DIR/bin
fi
export CC=wllvm
export CXX=wllvm++
