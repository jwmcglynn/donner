#!/usr/bin/env bash
# Runs the renderer bakeoff: one fresh process per (backend, scene, size) so process memory peaks
# and allocator state belong to a single measurement. Writes JSON Lines results, snapshots and host
# metadata into an output directory for tools/bakeoff_report.
#
# Usage (from benchmarks/renderer_bakeoff, after `bazel build //:renderer_bakeoff`):
#   tools/run_bakeoff.sh --out results/<run-name> [--suite gpu|software|all] [--sizes "512 1024"]
#       [--frames 50] [--warmup 10] [--backends "a b c"] [--scenes "x y"]
#
# The script performs no network access. It never retries a measurement on its own; host load and
# concurrently running CI jobs are recorded next to every measurement, and the report flags
# samples taken on a loaded host so they can be repeated deliberately.
set -euo pipefail

cd "$(dirname "$0")/.."

out=""
suite="all"
sizes="512 1024"
frames=50
warmup=10
backends=""
scenes=""
binary="${BAKEOFF_BINARY:-bazel-bin/renderer_bakeoff}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out) out="$2"; shift 2 ;;
    --suite) suite="$2"; shift 2 ;;
    --sizes) sizes="$2"; shift 2 ;;
    --frames) frames="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    --backends) backends="$2"; shift 2 ;;
    --scenes) scenes="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$out" ]]; then
  echo "--out DIR is required" >&2
  exit 2
fi
if [[ ! -x "$binary" ]]; then
  echo "benchmark binary not found at $binary; run: bazel build //:renderer_bakeoff" >&2
  exit 2
fi

mkdir -p "$out/png"
results="$out/results.jsonl"
loadlog="$out/host_load.tsv"
: > "$results"
printf 'timestamp\tbackend\tscene\tsize\tload_1m\tload_5m\tci_runner_jobs\texit\n' > "$loadlog"

"$binary" --describe > "$out/backends.json"

# Host metadata. Only generic hardware and toolchain facts are recorded.
{
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "os: $(uname -s) $(uname -r) $(uname -m)"
  if command -v sw_vers > /dev/null; then
    echo "macos: $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
    echo "cpu_cores: $(sysctl -n hw.ncpu)"
    echo "memory_bytes: $(sysctl -n hw.memsize)"
  else
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "cpu_cores: $(nproc)"
    echo "memory_kb: $(grep MemTotal /proc/meminfo | awk '{print $2}')"
  fi
  echo "compiler: $( (cc --version 2>/dev/null || true) | head -1)"
  echo "bazel: $(bazel --version 2>/dev/null || echo unknown)"
  echo "donner_revision: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "frames: $frames"
  echo "warmup: $warmup"
  echo "sizes: $sizes"
} > "$out/host.txt"

all_backends=$(python3 -c '
import json, sys
suite = sys.argv[1]
for b in json.load(open(sys.argv[2])):
    if b["suite"] == "reference" or b["name"] == "tiny_skia" or suite == "all" or b["suite"] == suite:
        print(b["name"])
' "$suite" "$out/backends.json")
if [[ -n "$backends" ]]; then
  all_backends="$backends"
fi

scene_args=()
for f in corpus/*.svg ../../donner_splash.svg ../../geode_splash.svg; do
  name=$(basename "$f" .svg)
  if [[ -z "$scenes" || " $scenes " == *" $name "* ]]; then
    scene_args+=("file:$f")
  fi
done
for name in $("$binary" --list-synthetic); do
  if [[ -z "$scenes" || " $scenes " == *" $name "* ]]; then
    scene_args+=("synthetic:$name")
  fi
done

if [[ ${#scene_args[@]} -eq 0 ]]; then
  echo "no scenes match --scenes \"$scenes\"" >&2
  exit 2
fi

ci_jobs() {
  # Counts GitHub Actions runner job processes, read-only, as a host-noise annotation.
  { pgrep -x Runner.Worker 2> /dev/null || true; } | wc -l | tr -d ' '
}

for size in $sizes; do
  for scene in "${scene_args[@]}"; do
    kind="${scene%%:*}"
    value="${scene#*:}"
    if [[ "$kind" == "file" ]]; then
      name=$(basename "$value" .svg)
      scene_flag=(--scene-file "$value")
    else
      name="$value"
      scene_flag=(--synthetic "$value")
    fi
    for backend in $all_backends; do
      mkdir -p "$out/png/$backend"
      load=$(sysctl -n vm.loadavg 2> /dev/null | awk '{print $2"\t"$3}' ||
        awk '{print $1"\t"$2}' /proc/loadavg)
      jobs=$(ci_jobs)
      status=0
      "$binary" --backend "$backend" "${scene_flag[@]}" --size "$size" --frames "$frames" \
        --warmup "$warmup" --out-json "$results" \
        --out-png "$out/png/$backend/${name}_${size}.png" 2>> "$out/stderr.log" || status=$?
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date -u +%H:%M:%S)" "$backend" "$name" "$size" \
        "$load" "$jobs" "$status" >> "$loadlog"
      echo "$backend $name $size exit=$status"
    done
  done
done
