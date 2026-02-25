# setup directories and environment for testing liberator
# and builds the target
set -e

export PROJECT=$(dirname "$(realpath "$0")")
export LIBFUZZ=$PROJECT/libfuzz
export TOOLS_DIR=$LIBFUZZ
export LLVM_DIR=/home/mashmallow/lib/llvm-16
export LLVM_COMPILER=clang

if [ ! -d "$PROJECT/targets/$TARGET_NAME" ]; then
  usage
  return 1
fi

echo "Setting up directories for local testing."

echo "[INFO] Creating ${LIBFUZZ} folder."
echo "[INFO] Creating ${LIBFUZZ}/targets."
echo "[INFO] Copying $TOOLS_DIR/condition_extractor to $TOOLS_DIR/condition_extractor."
echo "[INFO] Copying $PROJECT/tool/misc/extract_included_functions.py to $TOOLS_DIR/tool/misc"

mkdir -p $LIBFUZZ
mkdir -p $LIBFUZZ/targets
mkdir -p $TOOLS_DIR/condition_extractor
mkdir -p $TOOLS_DIR/tool/misc

echo "[INFO] Creating Libfuzz directories"
rsync -a --exclude=".git" $PROJECT/condition_extractor $TOOLS_DIR
cp $PROJECT/tool/misc/extract_included_functions.py $TOOLS_DIR/tool/misc
