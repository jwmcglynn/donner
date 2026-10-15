#!/usr/bin/env bash
# Builds every //code_size:code_size_* binary with identical size-oriented flags, then records the
# stripped executable size, the delta against code_size_baseline, the total static-archive bytes of
# the libraries the backend adds, and the external repositories it adds. Writes code_size.json.
#
# Usage (from benchmarks/renderer_bakeoff): tools/measure_code_size.sh --out results/<run-name>
# Extra Bazel flags (for example an output root) can be passed in BAZEL_FLAGS and
# BAZEL_STARTUP_FLAGS.
set -euo pipefail

cd "$(dirname "$0")/.."

out=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --out) out="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "$out" ]]; then
  echo "--out DIR is required" >&2
  exit 2
fi
mkdir -p "$out/code_size"

case "$(uname -s)" in
  Darwin) config=codesize_macos ;;
  Linux) config=codesize_linux ;;
  *) echo "unsupported OS" >&2; exit 2 ;;
esac

# shellcheck disable=SC2206
startup=(${BAZEL_STARTUP_FLAGS:-})
# shellcheck disable=SC2206
flags=(${BAZEL_FLAGS:-})
bazel_cmd() { bazel ${startup[@]+"${startup[@]}"} "$@"; }

targets=$(bazel_cmd query 'kind(cc_binary, //code_size:all)' 2> /dev/null | sort)
bazel_cmd build ${flags[@]+"${flags[@]}"} --config="$config" $targets

baseline_target=//code_size:code_size_baseline
file_size() { wc -c < "$1" | tr -d ' '; }

binary_path() {
  bazel_cmd cquery ${flags[@]+"${flags[@]}"} --config="$config" --output=files "$1" 2> /dev/null | head -1
}

strip_binary() {
  # Remove every symbol not needed to run: Bazel's --strip=always only drops debug info.
  case "$(uname -s)" in
    Darwin) strip "$1" ;;
    Linux) strip --strip-all "$1" ;;
  esac
}

baseline_bin=$(binary_path "$baseline_target")
"$baseline_bin" > /dev/null
cp "$baseline_bin" "$out/code_size/.baseline"
chmod u+w "$out/code_size/.baseline"
strip_binary "$out/code_size/.baseline"
baseline_bytes=$(file_size "$out/code_size/.baseline")
rm -f "$out/code_size/.baseline"

entries=()
for target in $targets; do
  name=${target#//code_size:code_size_}
  bin=$(binary_path "$target")
  cp "$bin" "$out/code_size/$name"
  chmod u+w "$out/code_size/$name"
  strip_binary "$out/code_size/$name"
  # Run once so a binary that links but cannot render is not reported as a valid size.
  run_status=0
  "$bin" > "$out/code_size/$name.run.txt" 2>&1 || run_status=$?
  bytes=$(file_size "$out/code_size/$name")
  delta=$((bytes - baseline_bytes))

  archives=$(bazel_cmd cquery ${flags[@]+"${flags[@]}"} --config="$config" --output=files \
    "kind('cc_library', deps($target) except deps($baseline_target))" 2> /dev/null |
    grep -E '\.a$' || true)
  archive_bytes=0
  for a in $archives; do
    if [[ -f "$a" ]]; then
      archive_bytes=$((archive_bytes + $(file_size "$a")))
    fi
  done
  closure=$(bazel_cmd cquery ${flags[@]+"${flags[@]}"} --config="$config" \
    "kind('cc_library', deps($target) except deps($baseline_target))" 2> /dev/null |
    sed -n 's#^@@\{0,1\}\([^/]*\)//.*#\1#p' | sed 's/^$/main/' | sort -u | tr '\n' ' ' |
    sed 's/ $//')
  libraries=$(bazel_cmd cquery ${flags[@]+"${flags[@]}"} --config="$config" \
    "kind('cc_library', deps($target) except deps($baseline_target))" 2> /dev/null | wc -l |
    tr -d ' ')
  entries+=("{\"backend\":\"$name\",\"stripped_bytes\":$bytes,\"delta_bytes\":$delta,\"archive_bytes\":$archive_bytes,\"libraries\":$libraries,\"closure\":\"${closure:-none}\",\"run_status\":$run_status}")
done

notes="Executables built with -c opt, -ffunction-sections -fdata-sections, dead-section \
elimination at link (${config}; on macOS also no exported symbols), LTO off, then fully stripped \
with the platform strip tool. Delta is against code_size_baseline \
(Donner parse, style, layout, traversal and the null recorder). Archive bytes sum the static \
libraries a backend adds beyond the baseline, before dead stripping. Closure lists the \
repositories those libraries come from."

{
  printf '{"notes":"%s","platform":"%s","entries":[' "$notes" "$(uname -sm)"
  (IFS=,; printf '%s' "${entries[*]}")
  printf ']}\n'
} > "$out/code_size.json"
echo "wrote $out/code_size.json"
