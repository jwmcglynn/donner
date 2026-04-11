#!/bin/bash -e
#
# Build the Doxygen site and stage the checked-in report artifacts.
#
# Doxygen generates the API docs under generated-doxygen/html/. The
# pre-generated binary-size HTML and the coverage archive live under
# docs/reports/ so that `doxygen Doxyfile` itself stays fast — we don't
# regenerate coverage on every docs build.
#
# After Doxygen runs we:
#   - copy docs/reports/binary-size/ verbatim into the output tree, and
#   - extract docs/reports/coverage.zip into the output tree.
#
# That way docs/build_report.md's link to
#   https://jwmcglynn.github.io/donner/reports/coverage/index.html
# and its relative link to reports/binary-size/binary_size_report.html
# both resolve on the deployed docs site.

cd "$(dirname "$0")/.."

doxygen Doxyfile

OUT_DIR="generated-doxygen/html"
REPORTS_SRC="docs/reports"
REPORTS_DST="$OUT_DIR/reports"

if [ ! -d "$OUT_DIR" ]; then
  echo "ERROR: expected Doxygen output at $OUT_DIR" >&2
  exit 1
fi

mkdir -p "$REPORTS_DST"

# Binary-size: a tiny directory of pre-rendered HTML/SVG. Copy as-is.
if [ -d "$REPORTS_SRC/binary-size" ]; then
  rm -rf "$REPORTS_DST/binary-size"
  cp -R "$REPORTS_SRC/binary-size" "$REPORTS_DST/binary-size"
  echo "Copied $REPORTS_SRC/binary-size/ → $REPORTS_DST/binary-size/"
else
  echo "Note: $REPORTS_SRC/binary-size/ not present; skipping." >&2
fi

# Coverage: shipped as a zip (otherwise the lcov tree is ~hundreds of small
# files that bloat the repo and make fuzzy file-name search noisy). Extract
# into the output tree so reports/coverage/index.html is reachable on the
# deployed docs site.
if [ -f "$REPORTS_SRC/coverage.zip" ]; then
  rm -rf "$REPORTS_DST/coverage"
  unzip -q "$REPORTS_SRC/coverage.zip" -d "$REPORTS_DST"
  echo "Extracted $REPORTS_SRC/coverage.zip → $REPORTS_DST/coverage/"
else
  echo "Note: $REPORTS_SRC/coverage.zip not present; skipping." >&2
  echo "      Regenerate with: python3 tools/generate_build_report.py --all --save docs/build_report.md" >&2
fi
