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
  # Force every output to materialize locally. On macOS, bloaty attribution runs
  # through dsymutil, which reads the per-.o debug map embedded in the unstripped
  # binary; without this those object files can stay in the cache unmaterialized
  # and attribution comes back empty.
  --remote_download_all
)
BLOATY_BIN="$(command -v bloaty || true)"

BAZEL_CONFIGS=(--config=binary-size)

if [[ "$(uname)" == "Darwin" ]]; then
  BAZEL_CONFIGS=(--config=macos-binary-size)
elif [[ "$(uname)" == "Linux" ]]; then
  BAZEL_CONFIGS=(--config=linux-binary-size)
fi
# Same size-optimized build plus ThinLTO. ThinLTO shrinks the stripped binaries
# further but drops the debug info bloaty needs, so it is measured separately
# (headline shipped size only, no per-compile-unit attribution).
LTO_CONFIGS=(--config=binary-size-lto)
if [[ "$(uname)" == "Darwin" ]]; then
  LTO_CONFIGS=(--config=macos-binary-size-lto)
elif [[ "$(uname)" == "Linux" ]]; then
  LTO_CONFIGS=(--config=linux-binary-size-lto)
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
bazel build "${BAZEL_QUIET_OPTIONS[@]}" "${BAZEL_LOCAL_OPTIONS[@]}" "${BAZEL_CONFIGS[@]}" //donner/svg/parser:svg_parser_tool //donner/svg/parser:svg_parser_tool.stripped
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


##
## ThinLTO shipped-size build (native).
##
## ThinLTO folds and inlines across translation units for a smaller shipped
## binary, but its debug info points at transient .thinlto.o files that dsymutil
## and bloaty cannot open, so it is built separately from the attribution build
## above and reported as headline sizes only. Set SKIP_LTO=1 to skip.
##
if [[ -z "$SKIP_LTO" ]]; then
  echo ""
  echo "### ThinLTO shipped native sizes (${LTO_CONFIGS[*]})"
  echo ""

  bazel build "${BAZEL_QUIET_OPTIONS[@]}" "${BAZEL_LOCAL_OPTIONS[@]}" "${LTO_CONFIGS[@]}" //donner/svg/parser:svg_parser_tool.stripped
  cp -f bazel-bin/donner/svg/parser/svg_parser_tool.stripped build-binary-size/svg_parser_tool.lto
  bazel build "${BAZEL_QUIET_OPTIONS[@]}" "${BAZEL_LOCAL_OPTIONS[@]}" "${LTO_CONFIGS[@]}" //donner/svg/tool:donner-svg.stripped
  cp -f bazel-bin/donner/svg/tool/donner-svg.stripped build-binary-size/donner-svg.lto

  parser_base=$(wc -c < build-binary-size/svg_parser_tool)
  parser_lto=$(wc -c < build-binary-size/svg_parser_tool.lto)
  donnersvg_base=$(wc -c < build-binary-size/donner-svg)
  donnersvg_lto=$(wc -c < build-binary-size/donner-svg.lto)

  parser_delta=$(awk "BEGIN{printf \"%.1f\", ($parser_lto-$parser_base)*100.0/$parser_base}")
  donnersvg_delta=$(awk "BEGIN{printf \"%.1f\", ($donnersvg_lto-$donnersvg_base)*100.0/$donnersvg_base}")

  echo '```'
  printf '%-16s %14s %14s %10s\n' "target" "non-LTO" "ThinLTO" "delta"
  printf '%-16s %14s %14s %9s%%\n' "svg_parser_tool" "$parser_base" "$parser_lto" "$parser_delta"
  printf '%-16s %14s %14s %9s%%\n' "donner-svg" "$donnersvg_base" "$donnersvg_lto" "$donnersvg_delta"
  echo '```'
fi
##
## WebAssembly transfer size (Emscripten).
##
## The .wasm + JS glue is the artifact a browser downloads, so the number that
## matters for the "downloadable as wasm" story is the gzip-compressed transfer
## size, not the on-disk size. Measure the tiny_skia SVG renderer wasm target
## (the minimal embedder surface: parser + svg + software renderer + a thin
## bridge). Skipped automatically when emcc is not on PATH.
##
if [[ -z "$SKIP_WASM" ]] && command -v emcc >/dev/null 2>&1; then
  # wasm-size is the size-optimized production config (see .bazelrc). Override
  # with WASM_CONFIG=wasm to measure the unoptimized dev build instead.
  WASM_CONFIG="${WASM_CONFIG:-wasm-size}"
  echo ""
  echo "### WebAssembly build (\`//donner/svg/renderer/wasm:donner_wasm\`, --config=$WASM_CONFIG)"
  echo ""

  bazel build "${BAZEL_QUIET_OPTIONS[@]}" "${BAZEL_LOCAL_OPTIONS[@]}" --config="$WASM_CONFIG" //donner/svg/renderer/wasm:donner_wasm

  cp -f bazel-bin/donner/svg/renderer/wasm/donner_wasm_bin.wasm build-binary-size/donner_wasm.wasm
  cp -f bazel-bin/donner/svg/renderer/wasm/donner_wasm_bin.js build-binary-size/donner_wasm.js

  wasm_raw=$(wc -c < build-binary-size/donner_wasm.wasm)
  wasm_gz=$(gzip -9 -c build-binary-size/donner_wasm.wasm | wc -c)
  js_raw=$(wc -c < build-binary-size/donner_wasm.js)
  js_gz=$(gzip -9 -c build-binary-size/donner_wasm.js | wc -c)

  echo '```'
  printf '%-24s %14s %14s\n' "artifact" "raw bytes" "gzip -9"
  printf '%-24s %14s %14s\n' "donner_wasm_bin.wasm" "$wasm_raw" "$wasm_gz"
  printf '%-24s %14s %14s\n' "donner_wasm_bin.js" "$js_raw" "$js_gz"
  printf '%-24s %14s %14s\n' "total transfer" "$((wasm_raw + js_raw))" "$((wasm_gz + js_gz))"
  echo '```'
  echo ""

  # Section-level attribution of the .wasm. Symbol attribution is best-effort:
  # it only appears when the module still carries a name section.
  echo '`bloaty -d sections,symbols -n 20` output for donner_wasm_bin.wasm'
  echo '```'
  run_bloaty -d sections,symbols -n 20 build-binary-size/donner_wasm.wasm || \
    run_bloaty -d sections -n 20 build-binary-size/donner_wasm.wasm || true
  echo '```'
else
  echo ""
  echo "_emcc not found on PATH (or SKIP_WASM set); skipping WebAssembly size measurement._"
fi
