#!/bin/bash

echo "TARGET_DIR: $TARGET_DIR"

if [ -z "$1" ]; then
  echo "Usage: copy_results <target>"
  exit 1
fi

# Source setup_target.sh to load configuration and environment variables
# This will set TARGET, TOOLS_DIR, etc.
source setup_target.sh "$1"

DEST="/mnt/c/Users/MaschPaul/Documents/$TARGET_NAME"_results.xlsx
WORK="$TARGET_DIR/$TARGET_NAME/work"

cp -r $WORK/results.xlsx $DEST
echo "Successfully copied $1 to $DEST!"
