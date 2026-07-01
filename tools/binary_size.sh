#!/bin/bash -e
# cd to the repo root
cd "$(bazel info workspace)"

mkdir -p build-binary-size

# If verbose is set, show all output
if [[ "$1" == "--verbose" ]]; then
  BAZEL_QUIET_OPTIONS=()
  set -ex
else
  # Bazel options used:
  # --incompatible_strict_action_env disables the warning when the analysis cache is discarded (when changing options such as compilation mode)
  # --ui_event_filters=-info,-stdout,-stderr --noshow_progress hides all compile output
  BAZEL_QUIET_OPTIONS=(--ui_event_filters=-info,-warning,-stdout,-stderr --noshow_progress)
fi

BAZEL_LOCAL_OPTIONS=(
  --remote_executor=
  --remote_cache=
  --noremote_upload_local_results
)
BLOATY_BIN="$(command -v bloaty || true)"

BAZEL_CONFIGS=(--config=binary-size)

if [[ "$(uname)" == "Darwin" ]]; then
  BAZEL_CONFIGS=(--config=macos-binary-size)
elif [[ "$(uname)" == "Linux" ]]; then
  BAZEL_CONFIGS=(--config=linux-binary-size)
fi

function run_bloaty() {
  if [[ -n "$BLOATY_BIN" ]]; then
    "$BLOATY_BIN" "$@"
  else
    bazel run -c opt "${BAZEL_QUIET_OPTIONS[@]}" "${BAZEL_LOCAL_OPTIONS[@]}" \
      --run_under="cd $PWD &&" @bloaty//:bloaty -- "$@"
  fi
}

# Build the binaries to analyze. Always measure the stripped outputs, and keep an
# unstripped parser binary around separately so bloaty can attribute sizes using symbols.
bazel build "${BAZEL_QUIET_OPTIONS[@]}" "${BAZEL_LOCAL_OPTIONS[@]}" "${BAZEL_CONFIGS[@]}" //donner/svg/parser:svg_parser_tool.stripped
cp -f bazel-bin/donner/svg/parser/svg_parser_tool.stripped build-binary-size/svg_parser_tool

bazel build "${BAZEL_QUIET_OPTIONS[@]}" "${BAZEL_LOCAL_OPTIONS[@]}" "${BAZEL_CONFIGS[@]}" //donner/svg/tool:donner-svg.stripped
cp -f bazel-bin/donner/svg/tool/donner-svg.stripped build-binary-size/donner-svg

# Print human-readable binary size of svg_parser_tool.stripped and donner-svg.stripped
echo '```'
echo "Total binary size of svg_parser_tool"
du -h build-binary-size/svg_parser_tool
echo ""
echo "Total binary size of donner-svg"
du -h build-binary-size/donner-svg
echo '```'
echo ""

echo "### Detailed analysis of \`svg_parser_tool\`"
echo ""

# On macOS run dsymutil to generate debug symbols. On Linux point bloaty at the
# unstripped Bazel output as the debug file while measuring the stripped binary.
DEBUG_FILE_ARG=()
if [[ "$(uname)" == "Darwin" ]]; then
  cp -f bazel-bin/donner/svg/parser/svg_parser_tool build-binary-size/svg_parser_tool.debug
  rm -rf build-binary-size/svg_parser_tool.debug.dSYM
  dsymutil build-binary-size/svg_parser_tool.debug
  DEBUG_FILE_ARG=(--debug-file=build-binary-size/svg_parser_tool.debug.dSYM/Contents/Resources/DWARF/svg_parser_tool.debug)
elif [[ "$(uname)" == "Linux" ]]; then
  cp -f bazel-bin/donner/svg/parser/svg_parser_tool build-binary-size/svg_parser_tool.debug
  DEBUG_FILE_ARG=(--debug-file=build-binary-size/svg_parser_tool.debug)
else
  echo "Unknown OS: $(uname)" >&2
  exit 1
fi

# See bloaty docs at https://github.com/google/bloaty/blob/main/doc/using.md
# To see file -> symbol tree: -d donner_package,compileunits,symbols
# To see symbols only: -d donner_package,symbol

# Output an <svg> with a bar chart of the binary size by directory
run_bloaty -c tools/binary_size_config.bloaty -d donner_package,compileunits -n 2000 --csv "${DEBUG_FILE_ARG[@]}" build-binary-size/svg_parser_tool > build-binary-size/svg_parser_tool.bloaty_compileunits.csv
python3 tools/python/generate_size_barchart_svg.py build-binary-size/svg_parser_tool.bloaty_compileunits.csv > build-binary-size/binary_size_bargraph.svg

echo ""

# Create the binary_size_report webtreemap
run_bloaty -c tools/binary_size_config.bloaty -d donner_package,compileunits,symbols -n 2000 --csv "${DEBUG_FILE_ARG[@]}" build-binary-size/svg_parser_tool > build-binary-size/svg_parser_tool.bloaty.csv
python3 tools/binary_size_analysis.py build-binary-size/svg_parser_tool.bloaty.csv build-binary-size/binary_size_report.html

# Output summary
echo ""
echo '`bloaty -d compileunits -n 20` output'
echo '```'

run_bloaty -c tools/binary_size_config.bloaty -d donner_package,compileunits -n 20 "${DEBUG_FILE_ARG[@]}" build-binary-size/svg_parser_tool

echo '```'
