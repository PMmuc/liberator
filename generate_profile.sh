#!/bin/bash
set -e
set -x

# Usage: ./generate_profile.sh <target_name> [--as-excel]

if [ -z "$1" ]; then
  echo "Usage: $0 <target_name> [--as-excel <file>]"
  exit 1
fi

# Source setup_target.sh to load configuration and environment variables
# This will set TARGET, TOOLS_DIR, etc.
source setup_target.sh "$1"

ANALYSIS_DIR=$PROJECT/analysis

if [[ ! -d $ANALYSIS_DIR ]]; then
  echo "Could not find directory $ANALYSIS_DIR"
  exit 1
fi

SAVE_AS_EXCEL=False
if [[ "$2" == "--as-excel" ]]; then
  SAVE_AS_EXCEL=True
fi

WORK="$ANALYSIS_DIR/$TARGET_NAME/work"
GMON_OUT="$WORK/apipass/gmon.out"
EXTRACTOR="$TOOLS_DIR/condition_extractor/build/bin/extractor"
OUTPUT_FILE="$WORK/apipass/analysis_profile.txt"

# Check if gmon.out exists
if [ ! -f "$GMON_OUT" ]; then
  echo "Error: $GMON_OUT not found."
  echo "Did you run the analysis with a standard build? Make sure 'gmon.out' was generated."
  echo "You may need to run './analysis.sh $1' first."
  exit 1
fi

# Check if extractor binary exists
if [ ! -f "$EXTRACTOR" ]; then
  echo "Error: Extractor binary not found at $EXTRACTOR"
  exit 1
fi

echo "Generating gprof profile for target '$1'..."
gprof "$EXTRACTOR" "$GMON_OUT" >"$OUTPUT_FILE"

echo "Success! Profile output saved to:"
echo "$OUTPUT_FILE"

if [ $SAVE_AS_EXCEL ]; then
  python3 $PROJECT/gprof_to_excel.py -i $WORK/apipass/analysis_profile.txt -o $WORK/apipass/results.xlsx
  echo "Success! Excel file saved to:"
  echo "$WORK/results.xlsx"
fi
