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
CONFIG_FILE="$TARGET_DIR/$TARGET_NAME/config.sh"

if [ ! -d "$PROJECT/targets/$TARGET_NAME" ]; then
  echo "Error: Target directory $PROJECT/targets/$TARGET_NAME does not exist."
  exit 1
fi

echo "Copying ${PROJECT}/targets/${TARGET_NAME} to ${TARGET_DIR}/$TARGET_NAME"
rsync -a --exclude=".git" $PROJECT/targets/$TARGET_NAME $TARGET_DIR/

if [ ! -f "$CONFIG_FILE" ]; then
  echo "Error: Config file $CONFIG_FILE not found."
  exit 1
fi

cd $TARGET_DIR

# Load common environment variables
# NOTE: if TOOLS_DIR is unset, I assume to find stuffs in LIBFUZZ folder
# This mimics the behavior of original scripts
if [ -z "$TOOLS_DIR" ]; then
  if [ -n "$LIBFUZZ" ]; then
    TOOLS_DIR=$LIBFUZZ
  else
    echo "Error: TOOLS_DIR or LIBFUZZ env variable must be set."
    exit 1
  fi
fi

# Default environment setup
export LLVM_COMPILER=clang
# Default LLVM_COMPILER_PATH if LLVM_DIR is set, otherwise rely on system path or config override
if [ -n "$LLVM_DIR" ]; then
  export LLVM_COMPILER_PATH=$LLVM_DIR/bin
fi
export CC=wllvm
export CXX=wllvm++
