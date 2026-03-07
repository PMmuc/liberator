#!/bin/bash

ROOT_DIR=$(dirname $(realpath extract_analysis.sh))
# 1. Define your base directories here
SOURCE_BASE="$ROOT_DIR/analysis/"
DEST_BASE="$ROOT_DIR/analysis/tmp"

# Create the main destination directory if it doesn't exist
mkdir -p "$DEST_BASE"

# 2. Iterate through each item in the source directory
for TARGET_PATH in "$SOURCE_BASE"/*; do
	# Check if the current item is actually a directory
    if [ -d "$TARGET_PATH" ]; then
        
        # Extract just the folder name (the $TARGET)
        TARGET=$(basename "$TARGET_PATH")
        APIPASS_DIR="$TARGET_PATH/work/apipass"
        
        # 3. Check if the 'work/apipass/' subfolder exists for this specific target
        if [ -d "$APIPASS_DIR" ]; then
            echo "Copying files for target: $TARGET"
            
            # Create the specific $DEST/$TARGET directory
            mkdir -p "$DEST_BASE/$TARGET"
            
            # 4. Copy all files and folders
            # The '/.' at the end of the source path is a neat trick: 
            # It ensures we copy the *contents* (including hidden files) 
            # rather than the 'apipass' folder itself.
            cp -a "$APIPASS_DIR"/. "$DEST_BASE/$TARGET/"
            
        else
            echo "Skipping $TARGET: 'work/apipass/' not found."
        fi
    fi
done

echo "All copying complete!"

scp -P 22222 -r paul@185.73.23.149:/home/paul/liberator/analysis/tmp /home/mashmallow/source/liberator/analysis/results
