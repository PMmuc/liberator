#!/bin/bash
set -e
set -x

export TARGET_NAME=${TARGET}
TARGET_DIR=${LIBFUZZ}/analysis/${TARGET_NAME}
WORK=${TARGET_DIR}/work
cd ${LIBFUZZ}
mkdir -p $TARGET_DIR
# Preinstall is handled by analysis.sh -> config.sh
# Fetch is handled by analysis.sh
echo "Start analysis"
# NOTE: write the timing file into TARGET_DIR, not WORK: analysis.sh does
# `rm -rf "$WORK"` at the start of a full run, which would unlink this file.
{ time ./analysis.sh ${TARGET_NAME}; } 2>${TARGET_DIR}/${TARGET_NAME}_analysis_time.txt
echo "Finished analysis"
