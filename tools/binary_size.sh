#!/bin/bash -e
# cd to the repo root
cd "$(bazel info workspace)"

mkdir -p build-binary-size

# If verbose is set, show all output
if [[ "$1" == "--verbose" ]]; then
  BAZEL_QUIET_OPTIONS=""
  set -ex
else
  # Bazel options used:
  # --incompatible_strict_action_env disables the warning when the analysis cache is discarded (when changing options such as compilation mode)
  # --ui_event_filters=-info,-stdout,-stderr --noshow_progress hides all compile output
  BAZEL_QUIET_OPTIONS="--ui_event_filters=-info,-warning,-stdout,-stderr --noshow_progress"
fi

BAZEL_CONFIGS="--config=binary-size"

# If we're macos also specify the macos-binary-size config
if [[ "$(uname)" == "Darwin" ]]; then
  BAZEL_CONFIGS="$BAZEL_CONFIGS --config=macos-binary-size"
fi

# Build the binary to analyze, xml_tool
bazel build $BAZEL_QUIET_OPTIONS $BAZEL_CONFIGS //donner/svg/xml:xml_tool.stripped
cp -f bazel-bin/donner/svg/xml/xml_tool build-binary-size/xml_tool

bazel build $BAZEL_QUIET_OPTIONS $BAZEL_CONFIGS //donner/svg/renderer:renderer_tool.stripped
cp -f bazel-bin/donner/svg/renderer/renderer_tool build-binary-size/renderer_tool

# Print human-readable binary size of xml_tool.stripped and renderer_tool.stripped
echo '```'
echo "Total binary size of xml_tool"
du -h build-binary-size/xml_tool
echo ""
echo "Total binary size of renderer_tool"
du -h build-binary-size/renderer_tool
echo '```'
echo ""

echo "### Detailed analysis of \`xml_tool\`"
echo ""

# On macOS run dsymutil to generate debug symbols
DEBUG_FILE_ARG=
if [[ "$(uname)" == "Darwin" ]]; then
  dsymutil build-binary-size/xml_tool
  DEBUG_FILE_ARG="--debug-file=build-binary-size/xml_tool.dSYM/Contents/Resources/DWARF/xml_tool"
elif [[ "$(uname)" == "Linux" ]]; then
  cp -f build-binary-size/xml_tool build-binary-size/xml_tool.debug
  chmod +w build-binary-size/xml_tool
  strip -g build-binary-size/xml_tool
  DEBUG_FILE_ARG="--debug-file=build-binary-size/xml_tool.debug"
else
  echo "Unknown OS: $(uname)" >&2
  exit 1
fi

# See bloaty docs at https://github.com/google/bloaty/blob/main/doc/using.md
# To see file -> symbol tree: -d donner_package,compileunits,symbols
# To see symbols only: -d donner_package,symbol

# Output an <svg> with a bar chart of the binary size by directory
bazel run -c opt $BAZEL_QUIET_OPTIONS --run_under="cd $PWD &&" @bloaty//:bloaty -- -c tools/binary_size_config.bloaty -d donner_package,compileunits -n 2000 --csv $DEBUG_FILE_ARG build-binary-size/xml_tool > build-binary-size/xml_tool.bloaty_compileunits.csv
python3 tools/python/generate_size_barchart_svg.py build-binary-size/xml_tool.bloaty_compileunits.csv > build-binary-size/binary_size_bargraph.svg

echo ""

# Create the binary_size_report webtreemap
bazel run -c opt $BAZEL_QUIET_OPTIONS --run_under="cd $PWD &&" @bloaty//:bloaty -- -c tools/binary_size_config.bloaty -d donner_package,compileunits,symbols -n 2000 --csv $DEBUG_FILE_ARG build-binary-size/xml_tool > build-binary-size/xml_tool.bloaty.csv
python3 tools/binary_size_analysis.py build-binary-size/xml_tool.bloaty.csv build-binary-size/binary_size_report.html

# Output summary
echo ""
echo '`bloaty -d compileunits -n 20` output'
echo '```'

bazel run -c opt $BAZEL_QUIET_OPTIONS --run_under="cd $PWD &&" @bloaty//:bloaty -- -c tools/binary_size_config.bloaty -d donner_package,compileunits -n 20 $DEBUG_FILE_ARG build-binary-size/xml_tool

echo '```'
