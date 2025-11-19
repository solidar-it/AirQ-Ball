#!/bin/bash

SKETCH_DIR="$1"
BUILD_DIR="$2"
BIN="$3"

PROJECT=$(basename "$SKETCH_DIR")

# Get version from the .ino file inside sketch folder
VERSION=$(grep '^#define VERSION' "$SKETCH_DIR"/*.ino | awk -F'"' '{print $2}')
DATE=$(date +"%Y-%m-%d")

OUTDIR="$SKETCH_DIR/build/${PROJECT}_${VERSION}_${DATE}"
mkdir -p "$OUTDIR"

NEWBIN="${OUTDIR}/${PROJECT}_v${VERSION}_${DATE}.bin"
cp "$BIN" "$NEWBIN"

mkdir -p "$SKETCH_DIR/build/latest"
cp "$NEWBIN" "$SKETCH_DIR/build/latest/"

# Create ZIP
ZIPFILE="$SKETCH_DIR/build/${PROJECT}_${VERSION}_${DATE}.zip"
cd "$OUTDIR" || exit
zip -r "$ZIPFILE" ./*.bin

echo "Build OK!"
echo "Output: $NEWBIN"
echo "ZIP created: $ZIPFILE"
