#!/usr/bin/env bash
set -e

# Change directory to the script's location
cd "$(dirname "$0")"

echo "Compiling C files in $(pwd)..."

for f in *.c; do
    [ -e "$f" ] || continue
    echo "Compiling $f..."
    clang -S -emit-llvm -g -O0 "$f" -o "${f%.c}.ll"
done

echo "Compilation complete."
