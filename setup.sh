# setup directories and environment for testing liberator
# and builds the target
set -e

export PROJECT=$(dirname "$(realpath "$0")")
if [ -z "$LIBFUZZ" ]; then
  export LIBFUZZ=$PROJECT/libfuzz
fi

if [ -z "$TOOLS_DIR" ]; then
  export TOOLS_DIR=$PROJECT
fi

#echo "[INFO] Creating ${LIBFUZZ} folder."
#echo "[INFO] Creating ${LIBFUZZ}/targets."

#mkdir -p $LIBFUZZ
#mkdir -p $LIBFUZZ/targets
#mkdir -p $TOOLS_DIR/condition_extractor
#mkdir -p $TOOLS_DIR/tool/misc

#echo "[INFO] Creating Libfuzz directories"
#rsync -a --exclude=".git" $PROJECT/condition_extractor $TOOLS_DIR
#cp $PROJECT/tool/misc/extract_included_functions.py $TOOLS_DIR/tool/misc
