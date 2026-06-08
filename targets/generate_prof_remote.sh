#!/bin/bash
set -e
set -x

SAVE_AS_EXCEL=False
if [ $2 == "--as-excel" ]; then
  SAVE_AS_EXCEL=True
fi

GMON_OUT="$TARGET/work/apipass/gmon.out"
EXTRACTOR="$HOME/condition_extractor/bin/extractor"
OUTPUT_FILE="$TARGET/work/apipass/analysis_profile.txt"

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
  python3 $LIBFUZZ/gprof_to_excel.py -i $TARGET/work/apipass/analysis_profile.txt -o $TARGET/work/apipass/results.xlsx
  echo "Success! Excel file saved to:"
  echo "$WORK/apipass/results.xlsx"
fi
