#!/bin/bash
#
# Copy the apipass results of every target into ./tmp/<target>/.
#
# Usage:
#   ./copy_apipass.sh [analysis_dir] [dest_dir]
#
# analysis_dir defaults to ./analysis (e.g. pass ./analysis_global instead),
# dest_dir defaults to ./tmp.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${1:-$ROOT/analysis}"
DEST_DIR="${2:-$ROOT/tmp}"

if [ ! -d "$SRC_DIR" ]; then
  echo "Source folder $SRC_DIR does not exist"
  exit 1
fi

for target_dir in "$SRC_DIR"/*/; do
  target="$(basename "$target_dir")"
  apipass="$target_dir/work/apipass"

  if [ ! -d "$apipass" ]; then
    echo "[-] $target: no apipass folder, skipping"
    continue
  fi

  dest="$DEST_DIR/$target"
  mkdir -p "$dest"
  cp -a "$apipass/." "$dest/"
  echo "[+] $target -> $dest"
done
