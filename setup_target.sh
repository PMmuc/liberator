# Function to print usage
usage() {
  echo "Usage: $0 <target_name>"
  echo "Available targets are listed in targets.txt"
  exit 1
}

source setup.sh

if [[ -z "$1" && -z "$TARGET" ]]; then
  usage
fi

export TARGET_NAME=$1
export TARGET_DIR=$LIBFUZZ/targets

if [ -d "$TARGET" ]; then
  TARGET_NAME=$(basename $TARGET)
else
  TARGET_NAME=$1
fi

export TARGET=$TARGET_DIR/$TARGET_NAME

echo "LIBFUZZ: $LIBFUZZ"

SCRIPT_DIR="$LIBFUZZ/"
TARGET_DIR="$SCRIPT_DIR/targets/"
CONFIG_FILE="$PROJECT/targets/$TARGET_NAME/config.sh"


# Default environment setup
export LLVM_COMPILER=clang
# Default LLVM_COMPILER_PATH if LLVM_DIR is set, otherwise rely on system path or config override
if [ -n "$LLVM_DIR" ]; then
  export LLVM_COMPILER_PATH=$LLVM_DIR/bin
fi
export CC=wllvm
export CXX=wllvm++
