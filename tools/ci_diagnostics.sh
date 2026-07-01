#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage:
  tools/ci_diagnostics.sh init <target-mode>
  tools/ci_diagnostics.sh bazel <step-name> <bazel command...>
  tools/ci_diagnostics.sh coverage <coverage command...>
  tools/ci_diagnostics.sh finalize

Environment:
  DONNER_CI_DIAGNOSTICS_DIR  Artifact directory. Defaults under RUNNER_TEMP.
  DONNER_CI_TARGETS          Whitespace-separated Bazel target list.
  DONNER_CI_TARGET_FALLBACK  Target selection mode/reason.
EOF
}

diagnostics_dir() {
  if [[ -n "${DONNER_CI_DIAGNOSTICS_DIR:-}" ]]; then
    printf '%s\n' "$DONNER_CI_DIAGNOSTICS_DIR"
    return
  fi

  printf '%s/donner-ci-diagnostics\n' "${RUNNER_TEMP:-/tmp}"
}

write_github_env() {
  if [[ -n "${GITHUB_ENV:-}" ]]; then
    printf '%s=%s\n' "$1" "$2" >> "$GITHUB_ENV"
  fi
}

targets_file() {
  printf '%s/target-list.txt\n' "$(diagnostics_dir)"
}

read_targets() {
  local file
  file="$(targets_file)"
  if [[ ! -s "$file" ]]; then
    echo "ERROR: target list is missing: $file" >&2
    exit 1
  fi
  mapfile -t TARGETS < "$file"
}

record_timing() {
  local step_name="$1"
  local start_epoch="$2"
  local end_epoch
  end_epoch="$(date +%s)"
  mkdir -p "$(diagnostics_dir)"
  printf '%s\t%s\n' "$step_name" "$((end_epoch - start_epoch))" \
    >> "$(diagnostics_dir)/step-timings.tsv"
}

collect_test_xml() {
  local step_dir="$1"
  local testlogs_dir output_dir
  testlogs_dir="$(bazelisk info bazel-testlogs 2>/dev/null || true)"
  if [[ -z "$testlogs_dir" || ! -d "$testlogs_dir" ]]; then
    return
  fi

  output_dir="$step_dir/testlogs"
  mkdir -p "$output_dir"
  python3 - "$testlogs_dir" "$output_dir" <<'PY'
import pathlib
import shutil
import sys

source_root = pathlib.Path(sys.argv[1])
output_root = pathlib.Path(sys.argv[2])
for xml_path in source_root.rglob("test.xml"):
    try:
        rel = xml_path.relative_to(source_root)
    except ValueError:
        continue
    dest = output_root / rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(xml_path, dest)
PY
}

run_and_capture() {
  local step_name="$1"
  shift

  local diag_dir step_dir start_epoch status
  diag_dir="$(diagnostics_dir)"
  step_dir="$diag_dir/$step_name"
  mkdir -p "$step_dir"

  read_targets
  local bazel_flags=()
  if [[ "$step_name" != "fetch" ]]; then
    bazel_flags=(
      "--profile=$step_dir/profile.gz"
      "--experimental_profile_include_target_label"
      "--experimental_profile_include_primary_output"
      "--experimental_profile_additional_tasks=remote_queue"
      "--experimental_profile_additional_tasks=remote_setup"
      "--experimental_profile_additional_tasks=remote_process_time"
      "--experimental_profile_additional_tasks=remote_execution"
      "--experimental_profile_additional_tasks=remote_cache_check"
      "--experimental_profile_additional_tasks=remote_download"
      "--experimental_profile_additional_tasks=local_execution"
      "--build_event_json_file=$step_dir/bep.json"
      "--execution_log_json_file=$step_dir/execution-log.json"
    )
  fi

  printf '%q ' "$@" "${bazel_flags[@]}" "${TARGETS[@]}" > "$step_dir/command.txt"
  printf '\n' >> "$step_dir/command.txt"

  start_epoch="$(date +%s)"
  set +e
  "$@" "${bazel_flags[@]}" "${TARGETS[@]}" 2>&1 | tee "$step_dir/bazel.log"
  status="${PIPESTATUS[0]}"
  set -e
  record_timing "$step_name" "$start_epoch"

  if [[ -f "$step_dir/profile.gz" ]]; then
    bazelisk analyze-profile "$step_dir/profile.gz" > "$step_dir/profile.txt" 2>&1 || true
  fi
  collect_test_xml "$step_dir"

  return "$status"
}

init() {
  local target_mode="$1"
  local diag_dir target_count
  diag_dir="$(diagnostics_dir)"
  mkdir -p "$diag_dir"
  write_github_env DONNER_CI_DIAGNOSTICS_DIR "$diag_dir"

  if [[ -z "${DONNER_CI_TARGETS:-}" ]]; then
    echo "ERROR: DONNER_CI_TARGETS is required" >&2
    exit 1
  fi

  python3 - "$DONNER_CI_TARGETS" "$(targets_file)" <<'PY'
import pathlib
import sys

targets = [target for target in sys.argv[1].split() if target]
pathlib.Path(sys.argv[2]).write_text("\n".join(targets) + "\n", encoding="utf-8")
PY

  target_count="$(grep -cve '^[[:space:]]*$' "$(targets_file)")"
  local fallback_value fallback_reason
  fallback_value="${DONNER_CI_TARGET_FALLBACK%%:*}"
  fallback_reason="${DONNER_CI_TARGET_FALLBACK#*:}"
  {
    printf 'workflow=%s\n' "${GITHUB_WORKFLOW:-}"
    printf 'job=%s\n' "${GITHUB_JOB:-}"
    printf 'run_id=%s\n' "${GITHUB_RUN_ID:-}"
    printf 'run_attempt=%s\n' "${GITHUB_RUN_ATTEMPT:-}"
    printf 'sha=%s\n' "${GITHUB_SHA:-}"
    printf 'ref=%s\n' "${GITHUB_REF:-}"
    printf 'event_name=%s\n' "${GITHUB_EVENT_NAME:-}"
    printf 'runner_name=%s\n' "${RUNNER_NAME:-}"
    printf 'runner_os=%s\n' "${RUNNER_OS:-}"
    printf 'runner_arch=%s\n' "${RUNNER_ARCH:-}"
    printf 'target_mode=%s\n' "$target_mode"
    printf 'target_fallback=%s\n' "${DONNER_CI_TARGET_FALLBACK:-}"
    printf 'target_count=%s\n' "$target_count"
  } > "$diag_dir/manifest.env"
  {
    printf 'mode=%s\n' "$target_mode"
    printf 'fallback=%s\n' "$fallback_value"
    printf 'reason=%s\n' "$fallback_reason"
    printf 'target_count=%s\n' "$target_count"
  } > "$diag_dir/target-selection.env"

  echo "CI diagnostics: $diag_dir"
  echo "Target mode: $target_mode"
  echo "Target count: $target_count"
  echo "First targets:"
  sed -n '1,40p' "$(targets_file)"
}

coverage() {
  local step_name="coverage"
  local diag_dir step_dir start_epoch status
  diag_dir="$(diagnostics_dir)"
  step_dir="$diag_dir/$step_name"
  mkdir -p "$step_dir"

  read_targets
  printf '%q ' "$@" "${TARGETS[@]}" > "$step_dir/command.txt"
  printf '\n' >> "$step_dir/command.txt"

  local coverage_flags=(
    "--profile=$step_dir/profile.gz"
    "--experimental_profile_include_target_label"
    "--experimental_profile_include_primary_output"
    "--experimental_profile_additional_tasks=remote_queue"
    "--experimental_profile_additional_tasks=remote_setup"
    "--experimental_profile_additional_tasks=remote_process_time"
    "--experimental_profile_additional_tasks=remote_execution"
    "--experimental_profile_additional_tasks=remote_cache_check"
    "--experimental_profile_additional_tasks=remote_download"
    "--experimental_profile_additional_tasks=local_execution"
    "--build_event_json_file=$step_dir/bep.json"
    "--execution_log_json_file=$step_dir/execution-log.json"
  )
  export DONNER_COVERAGE_BAZEL_FLAGS="${coverage_flags[*]}"

  start_epoch="$(date +%s)"
  set +e
  "$@" "${TARGETS[@]}" 2>&1 | tee "$step_dir/bazel.log"
  status="${PIPESTATUS[0]}"
  set -e
  record_timing "$step_name" "$start_epoch"

  if [[ -f "$step_dir/profile.gz" ]]; then
    bazelisk analyze-profile "$step_dir/profile.gz" > "$step_dir/profile.txt" 2>&1 || true
  fi
  collect_test_xml "$step_dir"

  return "$status"
}

finalize() {
  local diag_dir
  diag_dir="$(diagnostics_dir)"
  mkdir -p "$diag_dir"
  if [[ -f tools/ci_diagnostics_report.py ]]; then
    python3 tools/ci_diagnostics_report.py "$diag_dir" --output "$diag_dir/report.md" || true
  fi
}

main() {
  if [[ $# -lt 1 ]]; then
    usage
    exit 2
  fi

  local command="$1"
  shift
  case "$command" in
    init)
      if [[ $# -ne 1 ]]; then
        usage
        exit 2
      fi
      init "$1"
      ;;
    bazel)
      if [[ $# -lt 2 ]]; then
        usage
        exit 2
      fi
      local step_name="$1"
      shift
      run_and_capture "$step_name" "$@"
      ;;
    coverage)
      if [[ $# -lt 1 ]]; then
        usage
        exit 2
      fi
      coverage "$@"
      ;;
    finalize)
      finalize
      ;;
    *)
      usage
      exit 2
      ;;
  esac
}

main "$@"
