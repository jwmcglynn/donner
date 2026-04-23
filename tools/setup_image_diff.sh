#!/usr/bin/env bash
#
# Wire up Git's `textconv` helpers so binary-image diffs show useful
# text output (dimensions + EXIF metadata) in `git diff`, IDE diff
# views, and PR-review surfaces — instead of "Binary file not shown".
#
# Uses `exiftool` if available (richest output), falls back to the
# POSIX `file -b` command otherwise. `.gitattributes` already routes
# PNG/JPEG/WEBP/GIF through the `exif` / `imgdiff` diff drivers; this
# script just tells Git what command to run for those drivers.
#
# Safe to rerun; just overwrites the two local config keys.

set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "${repo_root}"

if command -v exiftool >/dev/null 2>&1; then
  echo "Configuring Git to use exiftool for image diffs..."
  git config diff.exif.textconv exiftool
  git config diff.imgdiff.textconv exiftool
  echo "  diff.exif.textconv    = $(git config diff.exif.textconv)"
  echo "  diff.imgdiff.textconv = $(git config diff.imgdiff.textconv)"
else
  echo "exiftool not found; falling back to 'file -b'."
  echo "(Install with 'brew install exiftool' on macOS or 'apt-get install libimage-exiftool-perl' on Debian/Ubuntu for richer metadata.)"
  git config diff.exif.textconv "file -b"
  git config diff.imgdiff.textconv "file -b"
  echo "  diff.exif.textconv    = $(git config diff.exif.textconv)"
  echo "  diff.imgdiff.textconv = $(git config diff.imgdiff.textconv)"
fi

echo
echo "Done. Try: git diff HEAD~1 -- '*.png'"
