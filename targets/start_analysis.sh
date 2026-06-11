#!/bin/bash
set -e
set -x

export TARGET_NAME=${TARGET}

cd ${LIBFUZZ}
# Preinstall is handled by analysis.sh -> config.sh
# Fetch is handled by analysis.sh
echo "Start analysis"
{ time ./analysis.sh ${TARGET_NAME}; } 2>${LIBFUZZ}/analysis/${TARGET_NAME}/${TARGET_NAME}_analysis_time.txt
echo "Finished analysis"
