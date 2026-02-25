#!/bin/bash
set -e
set -x

export TARGET_NAME=${TARGET}
export TARGET=${LIBFUZZ}/analysis/${TARGET}
# ENV TARGET_NAME ${target_name}

echo "[TOOLS_DIR] ${TOOLS_DIR}"
echo "[TARGET] ${TARGET}"
echo "[WLLVM] $(which wllvm)"

cd ${LIBFUZZ}
# Preinstall is handled by analysis.sh -> config.sh
# Fetch is handled by analysis.sh
echo "Start analysis"
{ time ./analysis.sh ${TARGET_NAME}; } 2>${LIBFUZZ}/${TARGET_NAME}_analysis_time.txt
echo "Finished analysis"
