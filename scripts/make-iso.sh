#!/bin/bash
#
# make-iso.sh -- Create bootable GRUB2 ISO for SemperOS-on-seL4 (XCP-ng)
#
# Usage: ./make-iso.sh <build-directory>
#
# Dependencies: grub-mkrescue, xorriso
#
# SPDX-License-Identifier: BSD-2-Clause
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${1:?Usage: $0 <build-directory>}"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
OUTPUT="$BUILD_DIR/semperos-sel4.iso"

KERNEL="$BUILD_DIR/images/kernel-x86_64-pc99"
CAPDL="$BUILD_DIR/images/capdl-loader-image-x86_64-pc99"

# Verify build artifacts exist
for f in "$KERNEL" "$CAPDL"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: $f not found. Run ninja first." >&2
        exit 1
    fi
done

# Check dependencies
for cmd in grub-mkrescue xorriso; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "ERROR: $cmd not found. Install with: sudo apt install grub-common xorriso" >&2
        exit 1
    fi
done

# Create staging directory
STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT

mkdir -p "$STAGING/boot/grub"

# Copy build artifacts
cp "$KERNEL" "$STAGING/boot/"
cp "$CAPDL" "$STAGING/boot/"

# Copy GRUB config
cp "$SCRIPT_DIR/grub.cfg" "$STAGING/boot/grub/grub.cfg"

# Create ISO
grub-mkrescue -o "$OUTPUT" "$STAGING" 2>/dev/null

echo "ISO created: $OUTPUT"
echo "Size: $(du -h "$OUTPUT" | cut -f1)"
