#!/bin/bash -e
# cd to the repo root
cd "$(bazel info workspace)"

mkdir -p build-binary-size

# Build the binary to analyze, xml_tool
bazel build -c opt --strip=never --copt=-g //src/svg/xml:xml_tool.stripped

cp -f bazel-bin/src/svg/xml/xml_tool build-binary-size/xml_tool

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

# Bloaty docs at https://github.com/google/bloaty/blob/main/doc/using.md
# To see file -> symbol tree: -d donner_package,compileunits,symbols
# To see symbols only: -d donner_package,symbol
bazel run --ui_event_filters=-info,-stdout,-stderr --noshow_progress --run_under="cd $PWD &&" @bloaty//:bloaty -- -c tools/binary_size_config.bloaty -d donner_package,compileunits,symbols -n 2000 --csv $DEBUG_FILE_ARG build-binary-size/xml_tool > build-binary-size/xml_tool.bloaty.csv

python3 tools/binary_size_analysis.py build-binary-size/xml_tool.bloaty.csv build-binary-size/binary_size_report.html

echo ""
echo "Showing summary"
echo ""

bazel run --ui_event_filters=-info,-stdout,-stderr --noshow_progress --run_under="cd $PWD &&" @bloaty//:bloaty -- -c tools/binary_size_config.bloaty -d donner_package,compileunits -n 10 $DEBUG_FILE_ARG build-binary-size/xml_tool

echo ""
echo "Saved binary size report to build-binary-size/binary_size_report.html"
