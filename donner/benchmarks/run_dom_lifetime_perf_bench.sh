#!/usr/bin/env bash
set -euo pipefail

# Capture DOM lifetime/concurrency benchmarks as JSON artifacts and print a
# compact summary. Direct usage:
#
#   BAZEL=tools/llm-bazel-wrap.sh donner/benchmarks/run_dom_lifetime_perf_bench.sh
#
# Or via Bazel:
#
#   bazel test -c opt //donner/benchmarks:dom_lifetime_perf_capture --test_output=all

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BAZEL_COMPILATION_MODE="${BAZEL_COMPILATION_MODE:-opt}"

if [[ "${DOM_LIFETIME_BENCH_SMOKE:-0}" == "1" ]]; then
  BENCHMARK_MIN_TIME="${DOM_LIFETIME_BENCH_MIN_TIME:-0.001s}"
  BENCHMARK_REPETITIONS="${DOM_LIFETIME_BENCH_REPETITIONS:-1}"
else
  BENCHMARK_MIN_TIME="${DOM_LIFETIME_BENCH_MIN_TIME:-0.5s}"
  BENCHMARK_REPETITIONS="${DOM_LIFETIME_BENCH_REPETITIONS:-3}"
fi

if [[ "${1:-}" == "--bazel-test" ]]; then
  if [[ "$#" -ne 5 ]]; then
    echo "Usage: $0 --bazel-test <compilation_mode> <handle_bin> <detached_bin> <snapshot_bin>" >&2
    exit 2
  fi

  COMPILATION_MODE="$2"
  HANDLE_BIN="$3"
  DETACHED_BIN="$4"
  SNAPSHOT_BIN="$5"
  OUT_DIR="${TEST_UNDECLARED_OUTPUTS_DIR:-${PWD}/dom_lifetime_perf}"

  if [[ "${COMPILATION_MODE}" != "opt" ]]; then
    echo "Skipping DOM lifetime perf capture: compilation mode is ${COMPILATION_MODE} (need opt)."
    exit 0
  fi

  RUN_MODE="binary"
else
  OUT_DIR="${1:-${ROOT_DIR}/.benchmarks/dom_lifetime/$(date +%Y%m%d-%H%M%S)}"
  read -r -a BAZEL_CMD <<< "${BAZEL:-bazel}"
  RUN_MODE="bazel"
fi

mkdir -p "${OUT_DIR}"

HANDLE_JSON="${OUT_DIR}/svg_element_handle_bench.json"
DETACHED_JSON="${OUT_DIR}/detached_subtree_collection_bench.json"
SNAPSHOT_JSON="${OUT_DIR}/render_snapshot_bench.json"
BUDGET_PATH="${DOM_LIFETIME_BENCH_BUDGET:-${ROOT_DIR}/donner/benchmarks/budgets/dom_lifetime_default_eligibility_budget.json}"
if [[ "${BUDGET_PATH}" != /* ]]; then
  BUDGET_PATH="${ROOT_DIR}/${BUDGET_PATH}"
fi
ENFORCE_BUDGETS="${DOM_LIFETIME_BENCH_ENFORCE_BUDGETS:-0}"

benchmark_args() {
  local json_out="$1"
  local filter="$2"

  local args=(
    "--benchmark_repetitions=${BENCHMARK_REPETITIONS}"
    "--benchmark_report_aggregates_only=true"
    "--benchmark_min_time=${BENCHMARK_MIN_TIME}"
    "--benchmark_time_unit=ns"
    "--benchmark_out=${json_out}"
    "--benchmark_out_format=json"
  )

  if [[ -n "${filter}" ]]; then
    args+=("--benchmark_filter=${filter}")
  fi

  printf '%s\n' "${args[@]}"
}

run_binary_benchmark() {
  local bin="$1"
  local json_out="$2"
  local filter="$3"
  local args=()

  while IFS= read -r arg; do
    args+=("${arg}")
  done < <(benchmark_args "${json_out}" "${filter}")

  "${bin}" "${args[@]}"
}

run_bazel_benchmark() {
  local target="$1"
  local json_out="$2"
  local filter="$3"
  local args=()

  while IFS= read -r arg; do
    args+=("${arg}")
  done < <(benchmark_args "${json_out}" "${filter}")

  "${BAZEL_CMD[@]}" run -c "${BAZEL_COMPILATION_MODE}" "${target}" -- "${args[@]}"
}

run_benchmark() {
  local label="$1"
  local target="$2"
  local bin="$3"
  local json_out="$4"
  local filter="$5"

  echo "Running ${label}..."
  if [[ "${RUN_MODE}" == "binary" ]]; then
    run_binary_benchmark "${bin}" "${json_out}" "${filter}"
  else
    run_bazel_benchmark "${target}" "${json_out}" "${filter}"
  fi
}

if [[ "${DOM_LIFETIME_BENCH_SMOKE:-0}" == "1" ]]; then
  HANDLE_FILTER="${DOM_LIFETIME_BENCH_HANDLE_FILTER:-^BM_SVGElementHandle_CopyDestroy_(SingleThreaded|ConcurrentDom)/100$}"
  DETACHED_FILTER="${DOM_LIFETIME_BENCH_DETACHED_FILTER:-^BM_DetachedSubtreeCollection_ReinsertBeforeCollection/100$}"
  SNAPSHOT_FILTER="${DOM_LIFETIME_BENCH_SNAPSHOT_FILTER:-^BM_RenderSnapshot_Capture/1000$}"
elif [[ "${DOM_LIFETIME_BENCH_FULL:-0}" == "1" ]]; then
  HANDLE_FILTER="${DOM_LIFETIME_BENCH_HANDLE_FILTER:-}"
  DETACHED_FILTER="${DOM_LIFETIME_BENCH_DETACHED_FILTER:-}"
  SNAPSHOT_FILTER="${DOM_LIFETIME_BENCH_SNAPSHOT_FILTER:-}"
else
  HANDLE_FILTER="${DOM_LIFETIME_BENCH_HANDLE_FILTER:-}"
  DETACHED_FILTER="${DOM_LIFETIME_BENCH_DETACHED_FILTER:-}"
  SNAPSHOT_FILTER="${DOM_LIFETIME_BENCH_SNAPSHOT_FILTER:-^BM_RenderSnapshot_(Capture|ReplayTinySkiaBackend)/(1000|10000)$}"
fi

echo "Output directory: ${OUT_DIR}"
echo "Benchmark settings: repetitions=${BENCHMARK_REPETITIONS}, min_time=${BENCHMARK_MIN_TIME}"
if [[ "${ENFORCE_BUDGETS}" == "1" ]]; then
  echo "Budget enforcement: ${BUDGET_PATH}"
fi
if [[ "${DOM_LIFETIME_BENCH_FULL:-0}" != "1" && "${DOM_LIFETIME_BENCH_SMOKE:-0}" != "1" ]]; then
  echo "Snapshot benchmark default excludes 100k elements; set DOM_LIFETIME_BENCH_FULL=1 to include it."
fi
echo ""

run_benchmark \
  "SVG element handle benchmark" \
  "//donner/benchmarks:svg_element_handle_bench" \
  "${HANDLE_BIN:-}" \
  "${HANDLE_JSON}" \
  "${HANDLE_FILTER}"

run_benchmark \
  "detached subtree collection benchmark" \
  "//donner/benchmarks:detached_subtree_collection_bench" \
  "${DETACHED_BIN:-}" \
  "${DETACHED_JSON}" \
  "${DETACHED_FILTER}"

run_benchmark \
  "render snapshot benchmark" \
  "//donner/benchmarks:render_snapshot_bench" \
  "${SNAPSHOT_BIN:-}" \
  "${SNAPSHOT_JSON}" \
  "${SNAPSHOT_FILTER}"

python3 - "${HANDLE_JSON}" "${DETACHED_JSON}" "${SNAPSHOT_JSON}" "${BUDGET_PATH}" "${ENFORCE_BUDGETS}" <<'PY'
import json
import sys


def load_results(path):
    with open(path, "r", encoding="utf-8") as input_file:
        data = json.load(input_file)

    results = {}
    for item in data.get("benchmarks", []):
        name = item.get("name", "")
        is_mean = item.get("run_type") == "aggregate" and item.get("aggregate_name") == "mean"
        if is_mean:
            if name.endswith("_mean"):
                name = name[:-5]
            results[name] = item
        elif not name.endswith(("_median", "_stddev", "_cv")):
            results[name] = item
    return results


def format_ns(value):
    if value is None:
        return "n/a"
    if value >= 1_000_000:
        return f"{value / 1_000_000:.2f} ms"
    if value >= 1_000:
        return f"{value / 1_000:.2f} us"
    return f"{value:.2f} ns"


def real_time(results, name):
    item = results.get(name)
    if item is None:
        return None
    return float(item["real_time"])


def print_ratio(results, label, concurrent_name, single_threaded_name):
    concurrent = real_time(results, concurrent_name)
    single_threaded = real_time(results, single_threaded_name)
    if concurrent is None or single_threaded is None:
        return

    ratio = concurrent / single_threaded
    print(
        f"  {label:<34} single={format_ns(single_threaded):>10} "
        f"concurrent={format_ns(concurrent):>10} ratio={ratio:.2f}x"
    )


def print_time(results, label, name):
    value = real_time(results, name)
    if value is None:
        return
    print(f"  {label:<46} {format_ns(value):>10}")


def print_lock_counters(results, label, name):
    item = results.get(name)
    if item is None:
        return

    counter_names = (
        "read_locks_per_iter",
        "write_locks_per_iter",
        "max_read_lock_ns",
        "max_write_lock_ns",
    )
    counters = []
    for counter_name in counter_names:
        if counter_name in item:
            counters.append(f"{counter_name}={float(item[counter_name]):.2f}")
    if counters:
        print(f"  {label:<34} {' '.join(counters)}")


def print_snapshot_counters(results, label, name):
    item = results.get(name)
    if item is None:
        return

    command_count = item.get("snapshot_commands")
    storage_bytes = item.get("snapshot_command_storage_bytes")
    if command_count is None and storage_bytes is None:
        return

    parts = []
    if command_count is not None:
        parts.append(f"commands={float(command_count):.0f}")
    if storage_bytes is not None:
        parts.append(f"command_storage_bytes={float(storage_bytes):.0f}")
    print(f"  {label:<46} {' '.join(parts)}")


def metric_value(all_results, check):
    kind = check.get("type")

    if kind == "ratio":
        numerator = real_time(all_results, check["numerator"])
        denominator = real_time(all_results, check["denominator"])
        if numerator is None or denominator is None:
            return None
        if denominator == 0:
            raise ValueError(f"Budget check {check['name']} has zero denominator")
        return numerator / denominator

    if kind == "real_time_ns":
        return real_time(all_results, check["benchmark"])

    if kind == "counter":
        item = all_results.get(check["benchmark"])
        if item is None or check["counter"] not in item:
            return None
        return float(item[check["counter"]])

    raise ValueError(f"Unknown budget check type: {kind}")


def check_budget(all_results, budget_path, enforce):
    if not enforce:
        return 0

    with open(budget_path, "r", encoding="utf-8") as budget_file:
        budget = json.load(budget_file)

    print("")
    print("Budget checks:")
    failures = 0
    evaluated = 0
    for check in budget.get("checks", []):
        name = check["name"]
        value = metric_value(all_results, check)
        if value is None:
            if check.get("required", False):
                print(f"  FAIL {name}: missing required benchmark data")
                failures += 1
            else:
                print(f"  SKIP {name}: benchmark data not present in this run")
            continue

        evaluated += 1
        status = "PASS"
        problems = []
        if "min" in check and value < float(check["min"]):
            status = "FAIL"
            problems.append(f"value {value:.6g} below min {float(check['min']):.6g}")
        if "max" in check and value > float(check["max"]):
            status = "FAIL"
            problems.append(f"value {value:.6g} above max {float(check['max']):.6g}")

        if status == "FAIL":
            failures += 1
            print(f"  FAIL {name}: {'; '.join(problems)}")
        else:
            print(f"  PASS {name}: value={value:.6g}")

    if evaluated == 0:
        print("  FAIL no budget checks matched this benchmark run")
        failures += 1

    if failures:
        print("")
        print(f"FAILED: {failures} DOM lifetime budget check(s) failed.")
        return 1

    print("")
    print("PASSED: DOM lifetime budget checks passed.")
    return 0


handle_results = load_results(sys.argv[1])
detached_results = load_results(sys.argv[2])
snapshot_results = load_results(sys.argv[3])
budget_path = sys.argv[4]
enforce_budget = sys.argv[5] == "1"
all_results = {}
all_results.update(handle_results)
all_results.update(detached_results)
all_results.update(snapshot_results)

print("")
print("DOM Lifetime Performance Summary")
print("================================")
print("")
print("Handle overhead ratios (ConcurrentDom / SingleThreaded):")
for size in (100, 1000, 10000):
    print_ratio(
        handle_results,
        f"CopyDestroy/{size}",
        f"BM_SVGElementHandle_CopyDestroy_ConcurrentDom/{size}",
        f"BM_SVGElementHandle_CopyDestroy_SingleThreaded/{size}",
    )
    print_ratio(
        handle_results,
        f"NextSiblingTraversal/{size}",
        f"BM_SVGElementHandle_NextSiblingTraversal_ConcurrentDom/{size}",
        f"BM_SVGElementHandle_NextSiblingTraversal_SingleThreaded/{size}",
    )
    print_ratio(
        handle_results,
        f"QuerySelector/{size}",
        f"BM_SVGElementHandle_QuerySelector_ConcurrentDom/{size}",
        f"BM_SVGElementHandle_QuerySelector_SingleThreaded/{size}",
    )
print_ratio(
    handle_results,
    "RemoveReinsert",
    "BM_SVGElementHandle_RemoveReinsert_ConcurrentDom",
    "BM_SVGElementHandle_RemoveReinsert_SingleThreaded",
)

print("")
print("Handle lock counters:")
for size in (100, 1000, 10000):
    print_lock_counters(
        handle_results,
        f"CopyDestroy/{size}",
        f"BM_SVGElementHandle_CopyDestroy_ConcurrentDom/{size}",
    )
    print_lock_counters(
        handle_results,
        f"NextSiblingTraversal/{size}",
        f"BM_SVGElementHandle_NextSiblingTraversal_ConcurrentDom/{size}",
    )
    print_lock_counters(
        handle_results,
        f"QuerySelector/{size}",
        f"BM_SVGElementHandle_QuerySelector_ConcurrentDom/{size}",
    )
print_lock_counters(
    handle_results,
    "RemoveReinsert",
    "BM_SVGElementHandle_RemoveReinsert_ConcurrentDom",
)
print_lock_counters(
    handle_results,
    "FinalReleaseAttached",
    "BM_SVGElementHandle_FinalReleaseAttached_ConcurrentDom",
)
print_lock_counters(
    handle_results,
    "FinalReleaseDetached",
    "BM_SVGElementHandle_FinalReleaseDetached_ConcurrentDom",
)

print("")
print("Detached subtree collection:")
for size in (100, 1000, 10000):
    print_time(
        detached_results,
        f"RetainedByDescendant/{size}",
        f"BM_DetachedSubtreeCollection_RetainedByDescendant/{size}",
    )
    print_time(
        detached_results,
        f"ReinsertBeforeCollection/{size}",
        f"BM_DetachedSubtreeCollection_ReinsertBeforeCollection/{size}",
    )

print("")
print("Render snapshot:")
for size in (1000, 10000, 100000):
    print_time(snapshot_results, f"Capture/{size}", f"BM_RenderSnapshot_Capture/{size}")
    print_time(
        snapshot_results,
        f"ReplayTinySkiaBackend/{size}",
        f"BM_RenderSnapshot_ReplayTinySkiaBackend/{size}",
    )

print("")
print("Render snapshot storage counters:")
for size in (1000, 10000, 100000):
    print_snapshot_counters(
        snapshot_results,
        f"Capture/{size}",
        f"BM_RenderSnapshot_Capture/{size}",
    )
    print_snapshot_counters(
        snapshot_results,
        f"ReplayTinySkiaBackend/{size}",
        f"BM_RenderSnapshot_ReplayTinySkiaBackend/{size}",
    )

sys.exit(check_budget(all_results, budget_path, enforce_budget))
PY

echo ""
echo "JSON artifacts:"
echo "  ${HANDLE_JSON}"
echo "  ${DETACHED_JSON}"
echo "  ${SNAPSHOT_JSON}"
echo ""
if [[ "${ENFORCE_BUDGETS}" == "1" ]]; then
  echo "Budget file:"
  echo "  ${BUDGET_PATH}"
else
  echo "No thresholds were enforced. Set DOM_LIFETIME_BENCH_ENFORCE_BUDGETS=1 to gate against:"
  echo "  ${BUDGET_PATH}"
fi
