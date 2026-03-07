#!/bin/bash
set -e
set -x

# Usage: ./generate_profile.sh <target_name>

if [ -z "$1" ]; then
  echo "Usage: $0 <target_name> [--as-excel <file>]"
  exit 1
fi

SAVE_AS_EXCEL=False
if [ $2 == "--as-excel" ]; then
  SAVE_AS_EXCEL=True
fi

TARGET=$1
ROOT_DIR=$(dirname $(realpath generate_profiling.sh))
WORK="$ROOT_DIR/analysis/$TARGET/work"
GMON_OUT="$WORK/gmon.out"
EXTRACTOR="$ROOT_DIR/condition_extractor/build/bin/extractor"
OUTPUT_FILE="$WORK/analysis_profile.txt"

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
  python3 $PROJECT/gprof_to_excel.py -i $WORK/analysis_profile.txt -o $WORK/results.xlsx
  echo "Success! Excel file saved to:"
  echo "$WORK/results.xlsx"
fi
