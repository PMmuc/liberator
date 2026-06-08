#!/bin/bash

# Ensure we are in the root directory
if [ ! -f "docker/run_analysis.sh" ]; then
    echo "[ERROR] Please run this script from the root directory of the repository."
    exit 1
fi

echo "[INFO] Starting batch analysis for all targets..."

for target_dir in targets/*; do
    if [ -d "$target_dir" ] && [ -f "$target_dir/analysis.sh" ]; then
        TARGET_NAME=$(basename "$target_dir")
        
        echo "========================================================="
        echo "[INFO] Running analysis for target: $TARGET_NAME"
        echo "========================================================="
        
        export TARGET="$TARGET_NAME"
        (cd docker && ./run_analysis.sh)
        
        if [ $? -ne 0 ]; then
            echo "[ERROR] Analysis failed for $TARGET_NAME"
        else
            echo "[SUCCESS] Analysis completed for $TARGET_NAME"
        fi
        
        echo ""
    fi
done

echo "[INFO] All target analyses have finished."
