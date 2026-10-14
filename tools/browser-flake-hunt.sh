#!/usr/bin/env bash
#
# browser-flake-hunt.sh - repeat editor Wasm browser cases against one built package.
#
# A browser CI run rebuilds the package and runs every lane, so one more sample of an
# intermittent failure costs a whole CI run. This serves one prebuilt package and runs only the
# chosen cases, many times, with Playwright's --repeat-each, optionally on a host made to behave
# more like a loaded CI runner. Each failure keeps its own Playwright output directory: the error
# context and, for cases armed with failure evidence, failure-canvas-evidence.json. The console
# lines of the whole run are kept in playwright.log. Nothing has to be rerun to look at a failure.
#
# Usage:
#   tools/browser-flake-hunt.sh --package DIR --out DIR [options] [-- playwright args...]
#
# Options:
#   --package DIR      Prebuilt editor package to serve (a wasm_web_package output or a CI
#                      package artifact). Required; nothing is built.
#   --out DIR          Where results, evidence and the summary go. Required.
#   --config FILE      Playwright config, relative to donner/editor/wasm/tests
#                      (default: playwright.config.js).
#   --project NAME     Playwright project to run (default: all projects of the config).
#   --grep REGEX       Only cases whose title matches.
#   --repeat N         Runs per selected case (default: 10).
#   --cpu-load N       Keep N busy processes running alongside, so the browser competes for CPU
#                      as it does on a loaded runner (default: 0). Takes cores from everything
#                      else on the host, so use it only on a machine you have to yourself.
#   --low-priority     Run the Playwright process tree at background priority: taskpolicy -b on
#                      macOS, which also confines it to efficiency cores, or nice -n 19
#                      elsewhere. A browser helper process the system starts on the browser's
#                      behalf may not inherit it.
#
# Browsers run headed, as every CI browser lane does. Arguments after -- go to Playwright
# unchanged. The browser suites scale their bounds by the CI time scale, so CI=true is set unless
# the caller sets CI. Cases that compare bitmaps need the native comparator that
# tools/run-browser-ci.sh builds; export DONNER_BROWSER_GOLDEN_COMPARE to run them.
#
# The summary (summary.txt in --out) counts runs, passes and failures per case, prints each
# failure's first error line and output directory, and reports runs per hour.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd "${script_dir}/.." && pwd -P)"
readonly kTestsDir="${repo_root}/donner/editor/wasm/tests"

usage() {
  sed -n '3,38p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

die() {
  echo "error: $*" >&2
  exit 2
}

package_dir=""
out_dir=""
config="playwright.config.js"
project=""
grep_pattern=""
repeat=10
cpu_load=0
low_priority=0
extra_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --package|--out|--config|--project|--grep|--repeat|--cpu-load)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      case "$1" in
        --package) package_dir="$2" ;;
        --out) out_dir="$2" ;;
        --config) config="$2" ;;
        --project) project="$2" ;;
        --grep) grep_pattern="$2" ;;
        --repeat) repeat="$2" ;;
        --cpu-load) cpu_load="$2" ;;
      esac
      shift 2
      ;;
    --low-priority) low_priority=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; extra_args=("$@"); break ;;
    *) usage >&2; die "unknown argument: $1" ;;
  esac
done
[[ -n "${package_dir}" && -n "${out_dir}" ]] || die "--package and --out are required"
[[ "${repeat}" =~ ^[1-9][0-9]*$ ]] || die "--repeat must be a positive integer"
[[ "${cpu_load}" =~ ^(0|[1-9][0-9]*)$ ]] || die "--cpu-load must be a non-negative integer"
[[ -f "${package_dir}/editor.wasm" ]] || die "${package_dir} has no editor.wasm"
package_dir="$(cd "${package_dir}" && pwd -P)"
mkdir -p "${out_dir}"
out_dir="$(cd "${out_dir}" && pwd -P)"
[[ "${out_dir}" != "/" ]] || die "--out must not be the root directory"
pkg_copy="${out_dir}/package"
# The copy replaces ${pkg_copy}, so the package may not be in it, and --out may not be in the
# package that is copied.
[[ "${package_dir}/" != "${pkg_copy}/"* ]] || die "--package must not be in ${pkg_copy}"
[[ "${out_dir}/" != "${package_dir}/"* ]] || die "--out must not be in the package"

if [[ ! -d "${kTestsDir}/node_modules" ]]; then
  npm --prefix "${kTestsDir}" ci
fi

server_pid=""
load_pids=()
# shellcheck disable=SC2329  # invoked by the EXIT trap below.
cleanup() {
  local pid
  for pid in ${load_pids[@]+"${load_pids[@]}"}; do
    kill "${pid}" 2>/dev/null || true
  done
  if [[ -n "${server_pid}" ]]; then
    kill "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
  fi
}
trap cleanup EXIT
# Turn an interrupt or termination into an ordinary exit, so the cleanup above still runs.
trap 'exit 130' INT
trap 'exit 143' TERM

# Serve a private copy, as tools/run-browser-ci.sh does, on a free port.
rm -rf "${pkg_copy}"
cp -RL "${package_dir}" "${pkg_copy}"
chmod -R u+w "${pkg_copy}"
port="$(python3 - <<'PY'
import socket

with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    print(probe.getsockname()[1])
PY
)"
PYTHONUNBUFFERED=1 python3 "${repo_root}/tools/http/simple_webserver.py" --no-https \
  --host 127.0.0.1 --port "${port}" --dir "${pkg_copy}" >"${out_dir}/server.log" 2>&1 &
server_pid=$!
server_url="http://127.0.0.1:${port}"
ready=0
for _ in $(seq 1 100); do
  if curl -fsS -o /dev/null "${server_url}/index.html" 2>/dev/null; then
    ready=1
    break
  fi
  kill -0 "${server_pid}" 2>/dev/null || break
  sleep 0.1
done
if [[ "${ready}" != "1" ]]; then
  cat "${out_dir}/server.log" >&2
  die "the package server did not come up"
fi

for ((i = 0; i < cpu_load; i++)); do
  python3 -c 'while True: pass' &
  load_pids+=("$!")
done

export DONNER_WASM_BASE_URL="${server_url}"
export DONNER_WASM_BACKEND="geode"
export DONNER_WASM_REQUIRE_WEBGPU="1"
export DONNER_WASM_EXPECTED_HEADLESS_BACKEND="browser"
export CI="${CI:-true}"
export PLAYWRIGHT_JSON_OUTPUT_NAME="${out_dir}/report.json"
rm -f "${PLAYWRIGHT_JSON_OUTPUT_NAME}"

command=(npx playwright test "--config=${config}" "--repeat-each=${repeat}" --retries=0
         --workers=1 "--reporter=line,json" "--output=${out_dir}/results")
if [[ -n "${project}" ]]; then
  command+=("--project=${project}")
fi
if [[ -n "${grep_pattern}" ]]; then
  command+=("--grep=${grep_pattern}")
fi
command+=(--headed)
command+=(${extra_args[@]+"${extra_args[@]}"})
if [[ "${low_priority}" == "1" ]]; then
  if [[ "$(uname -s)" == "Darwin" ]]; then
    command=(taskpolicy -b "${command[@]}")
  else
    command=(nice -n 19 "${command[@]}")
  fi
fi

echo "Serving ${package_dir} at ${server_url}; ${repeat} runs per case, cpu-load ${cpu_load}," \
  "low-priority ${low_priority}"
started_at="$(date +%s)"
status=0
(cd "${kTestsDir}" && "${command[@]}") 2>&1 | tee "${out_dir}/playwright.log" || status=$?
elapsed_s=$(( $(date +%s) - started_at ))

if [[ ! -f "${PLAYWRIGHT_JSON_OUTPUT_NAME}" ]]; then
  echo "error: Playwright wrote no report; see ${out_dir}/playwright.log" >&2
  exit "$(( status == 0 ? 1 : status ))"
fi

python3 - "${out_dir}" "${elapsed_s}" <<'PY' | tee "${out_dir}/summary.txt"
import collections
import json
import os
import re
import sys

out_dir, elapsed_s = sys.argv[1], max(1, int(sys.argv[2]))
with open(os.path.join(out_dir, "report.json")) as handle:
    report = json.load(handle)
ansi = re.compile(r"\x1b\[[0-9;]*m")

counts = collections.defaultdict(collections.Counter)
failures = []

def visit(suite, titles):
    for spec in suite.get("specs", []):
        for test in spec.get("tests", []):
            project = test.get("projectName", "")
            name = " > ".join(titles + [spec["title"]]) + (f" [{project}]" if project else "")
            for result in test.get("results", []):
                counts[name][result["status"]] += 1
                if result["status"] in ("passed", "skipped"):
                    continue
                errors = result.get("errors") or [result.get("error") or {}]
                message = ansi.sub("", (errors[0] or {}).get("message", "")).strip()
                directories = sorted({
                    os.path.dirname(attachment["path"])
                    for attachment in result.get("attachments", [])
                    if attachment.get("path")
                })
                first_line = message.splitlines()[0] if message else ""
                failures.append((name, result["status"], first_line, directories))
    for child in suite.get("suites", []):
        visit(child, titles + ([child["title"]] if child.get("title") else []))

for suite in report.get("suites", []):
    visit(suite, [suite["title"]] if suite.get("title") else [])

total = sum(sum(counter.values()) for counter in counts.values())
print(f"runs: {total} in {elapsed_s} s, {total * 3600 // elapsed_s} runs per hour")
for name, counter in sorted(counts.items()):
    print(f"  {dict(counter)}  {name}")
print(f"failures: {len(failures)}")
for name, status, message, directories in failures:
    print(f"- {status}: {name}\n    {message}")
    for directory in directories:
        print(f"    output: {directory}")
PY
exit "${status}"
