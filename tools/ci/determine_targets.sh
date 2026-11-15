#!/usr/bin/env bash
#
# determine_targets.sh - the CI workflow's target selection: which Bazel targets a run builds and
# tests, and which optional lanes it needs. Each subcommand is one step of the workflow's
# determine-targets job and writes that step's outputs to $GITHUB_OUTPUT.
#
# Usage: tools/ci/determine_targets.sh pr-meta|determine|select-wgpu-reference
#        bazel run //tools/ci:plan -- pr-meta|determine|select-wgpu-reference
#
#   pr-meta                The revision whose bazel-diff hashes the job restores or saves
#                          (`base_sha`): a pull request's base, or this commit on a main push.
#   determine              The affected targets (`fallback`, `affected`, `full_test`) and the
#                          hints that gate optional lanes (`browser_affected`,
#                          `metal_affected` and the `wgpu_reference_*` outputs).
#   select-wgpu-reference  Whether a pull request runs the Linux WebGPU resvg reference (`run`),
#                          from the `determine` outputs passed in WGPU_REFERENCE_AFFECTED,
#                          WGPU_REFERENCE_CHANGED, WGPU_REFERENCE_UNKNOWN and FULL_TEST (each
#                          may be unset, which reads as empty).
#
# Run from the workspace checkout. The subcommands read the GitHub Actions default environment
# (GITHUB_EVENT_NAME, GITHUB_EVENT_PATH, GITHUB_REF, GITHUB_SHA, GITHUB_WORKSPACE, RUNNER_TEMP).
# `determine` runs the bazel-diff JAR that tools/ci/fetch_bazel_diff.sh saved in $RUNNER_TEMP,
# java, git and bazelisk.

# Each output is appended as its own line, in the order the selection decides it.
# shellcheck disable=SC2129

set -euo pipefail

# `bazel run` starts in the runfiles tree; the selection reads the checkout.
if [[ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]]; then
  cd "${BUILD_WORKSPACE_DIRECTORY}"
fi

pr_meta() {
  base_sha=""
  if [[ "$GITHUB_EVENT_NAME" == "pull_request" ]]; then
    base_sha="$(python3 -c 'import json, os; print(json.load(open(os.environ["GITHUB_EVENT_PATH"], encoding="utf-8")).get("pull_request", {}).get("base", {}).get("sha", ""))')"
  elif [[ "$GITHUB_REF" == "refs/heads/main" ]]; then
    # Pre-warm the cache for future PRs whose base is this main commit.
    base_sha="$GITHUB_SHA"
  fi
  echo "base_sha=$base_sha" >> "$GITHUB_OUTPUT"
}

determine() {
  if [[ "$GITHUB_EVENT_NAME" != "pull_request" ]]; then
    echo "Non-PR event; keeping full //... coverage."
    echo "fallback=true" >> "$GITHUB_OUTPUT"
    echo "affected=" >> "$GITHUB_OUTPUT"
    echo "full_test=false" >> "$GITHUB_OUTPUT"

    if [[ "$GITHUB_REF" == "refs/heads/main" ]]; then
      cached="$HOME/.cache/bazel-diff-base-hashes/$GITHUB_SHA.json"
      if [[ ! -f "$cached" ]]; then
        echo "Pre-warming bazel-diff hash cache for $GITHUB_SHA."
        mkdir -p "$(dirname "$cached")"
        java -jar "$RUNNER_TEMP/bazel-diff.jar" generate-hashes \
    --includeTargetType \
          -w "$GITHUB_WORKSPACE" \
          -b bazelisk \
          "$cached" || rm -f "$cached"
      fi
    fi
    exit 0
  fi

  eval "$(
    python3 -c 'import json, os, shlex; event = json.load(open(os.environ["GITHUB_EVENT_PATH"], encoding="utf-8")); pr = event.get("pull_request", {}); labels = [label.get("name", "") for label in pr.get("labels", [])]; values = {"BASE_SHA": pr.get("base", {}).get("sha", ""), "HEAD_SHA": pr.get("head", {}).get("sha", ""), "FORCE_FULL_TEST": "true" if "ci:full-test" in labels else "false"}; print("\n".join(f"{key}={shlex.quote(value)}" for key, value in values.items()))'
  )"

  if [[ -z "$BASE_SHA" || -z "$HEAD_SHA" ]]; then
    echo "Missing PR base/head SHA; falling back to //...."
    echo "fallback=true" >> "$GITHUB_OUTPUT"
    echo "affected=" >> "$GITHUB_OUTPUT"
    echo "full_test=false" >> "$GITHUB_OUTPUT"
    echo "wgpu_reference_unknown=true" >> "$GITHUB_OUTPUT"
    exit 0
  fi

  if [[ "$FORCE_FULL_TEST" == "true" ]]; then
    echo "ci:full-test label present; forcing full //... coverage."
    echo "fallback=true" >> "$GITHUB_OUTPUT"
    echo "affected=" >> "$GITHUB_OUTPUT"
    echo "full_test=true" >> "$GITHUB_OUTPUT"
    exit 0
  fi

  echo "full_test=false" >> "$GITHUB_OUTPUT"

  # Diff from the merge-base, not the base tip. The event payload's
  # base.sha is the base branch's CURRENT tip, so a two-dot diff
  # attributes post-fork drift on main (e.g. a workflow file merged
  # after this branch forked) to the PR, tripping the infra detector
  # and forcing a spurious full //... fallback. Merge-base semantics
  # sees only branch-side changes. Fail closed: if merge-base cannot
  # be computed, keep the base-tip diff (over-inclusion only ever
  # widens to the fallback path, never skips it).
  if diff_base="$(git merge-base "$BASE_SHA" "$HEAD_SHA")"; then
    echo "Using merge-base $diff_base as diff basis."
  else
    echo "git merge-base failed; using base tip $BASE_SHA as diff basis."
    diff_base="$BASE_SHA"
  fi
  changed_files="$(git diff --name-only "$diff_base" "$HEAD_SHA")"

  if [[ -z "$changed_files" ]]; then
    echo "No changed files detected; falling back to //...."
    echo "fallback=true" >> "$GITHUB_OUTPUT"
    echo "affected=" >> "$GITHUB_OUTPUT"
    echo "wgpu_reference_unknown=true" >> "$GITHUB_OUTPUT"
    exit 0
  fi

  # Workflow logic kept in scripts so the workflow files stay thin. A change to one of
  # them changes what CI runs exactly as a workflow edit does, so it selects the same
  # coverage as a change to this workflow. List only scripts moved out of a workflow.
  is_workflow_logic() {
    case "$1" in
      tools/ci/bazel_watchdog.sh|tools/ci/determine_targets.sh|tools/ci/fetch_bazel_diff.sh)
        return 0
        ;;
    esac
    return 1
  }

  # Direct renderer/GPU/image inputs and this lane's workflow still select the oracle
  # when an infra change makes bazel-diff fall back before it can report affected targets.
  wgpu_reference_changed=false
  while IFS= read -r path; do
    if is_workflow_logic "$path"; then
      wgpu_reference_changed=true
      break
    fi
    case "$path" in
      donner/svg/renderer/*|donner/gpu/*|donner/svg/resources/*|donner/svg/components/resources/*)
        wgpu_reference_changed=true
        break
        ;;
      donner/svg/SVGImageElement.*|donner/svg/SVGFEImageElement.*|third_party/resvg-test-suite/*)
        wgpu_reference_changed=true
        break
        ;;
      third_party/bazel/non_bcr_deps.bzl|third_party/BUILD.wgpu_native*|MODULE.bazel|MODULE.bazel.lock|.bazelrc|build_defs/*|.github/workflows/main.yml)
        wgpu_reference_changed=true
        break
        ;;
    esac
  done <<< "$changed_files"
  echo "wgpu_reference_changed=$wgpu_reference_changed" >> "$GITHUB_OUTPUT"

  # The browser tests' npm lockfile and Playwright browser manifest reach Bazel
  # through module extensions, not as files in the build graph, so bazel-diff
  # cannot attribute a change to them. A change to them adds every browser test to
  # the selection below. (The tests list package.json as a data file, so bazel-diff
  # sees it, but a dependency bump changes it together with the lockfile.)
  browser_inputs_changed=false
  while IFS= read -r path; do
    case "$path" in
      donner/editor/wasm/tests/package.json|donner/editor/wasm/tests/package-lock.json|donner/editor/wasm/tests/pnpm-lock.yaml|donner/editor/wasm/tests/browsers.*.json)
        echo "Browser test dependency change selects the browser partitions: $path"
        browser_inputs_changed=true
        break
        ;;
    esac
  done <<< "$changed_files"

  should_fallback=false
  while IFS= read -r path; do
    [[ -z "$path" ]] && continue
    if is_workflow_logic "$path"; then
      echo "Infra change requires full //... coverage: $path"
      should_fallback=true
      break
    fi
    case "$path" in
      MODULE.bazel|.bazelversion|WORKSPACE|WORKSPACE.*|.bazelrc|build_defs/*|.github/workflows/*|.github/actions/*)
        echo "Infra change requires full //... coverage: $path"
        should_fallback=true
        break
        ;;
    esac
  done <<< "$changed_files"

  if [[ "$should_fallback" == "true" ]]; then
    echo "fallback=true" >> "$GITHUB_OUTPUT"
    echo "affected=" >> "$GITHUB_OUTPUT"
    exit 0
  fi

  work_dir="$(mktemp -d)"
  base_dir="$work_dir/base"
  head_dir="$work_dir/head"
  cleanup() {
    git worktree remove --force "$base_dir" 2>/dev/null || true
    git worktree remove --force "$head_dir" 2>/dev/null || true
    rm -rf "$work_dir"
  }
  trap cleanup EXIT

  # Hash the current base TIP against the PR's MERGE commit (the
  # workspace checkout, refs/pull/N/merge), not against HEAD_SHA.
  # Hashing base-tip vs HEAD_SHA attributes post-fork drift on main to
  # the PR, inflating the affected set on any branch that has not
  # rebased (measured up to 126x over-inclusion; see
  # docs/design_docs/0029-2). In the tip-vs-merge comparison, drift is
  # present on BOTH sides, so unchanged-by-the-PR targets hash
  # identically and drop out; and because the merge graph is the one
  # hashed, reverse dependencies that exist ONLY in the merge (a dep
  # edge added on main after the fork onto code this PR touches) are
  # still caught - the downstream jobs build this same merge ref.
  # Fail closed: if the workspace is not a merge commit (e.g. a
  # checkout override), it IS the PR head and this degrades to exactly
  # the old base-tip-vs-head comparison.
  merge_sha="$(git rev-parse HEAD)"
  git worktree add --detach "$head_dir" "$merge_sha"

  cached_base="$HOME/.cache/bazel-diff-base-hashes/$BASE_SHA.json"
  if [[ -s "$cached_base" ]]; then
    echo "Using cached bazel-diff base hashes for $BASE_SHA."
    cp "$cached_base" "$work_dir/base-hashes.json"
  else
    git worktree add --detach "$base_dir" "$BASE_SHA"
    java -jar "$RUNNER_TEMP/bazel-diff.jar" generate-hashes \
    --includeTargetType \
      -w "$base_dir" \
      -b bazelisk \
      "$work_dir/base-hashes.json"
    # Save for subsequent pushes to this PR (post-job cache step).
    mkdir -p "$(dirname "$cached_base")"
    cp "$work_dir/base-hashes.json" "$cached_base"
  fi

  java -jar "$RUNNER_TEMP/bazel-diff.jar" generate-hashes \
    --includeTargetType \
    -w "$head_dir" \
    -b bazelisk \
    "$work_dir/head-hashes.json"

  java -jar "$RUNNER_TEMP/bazel-diff.jar" get-impacted-targets \
    -tt Rule \
    -w "$head_dir" \
    -b bazelisk \
    -sh "$work_dir/base-hashes.json" \
    -fh "$work_dir/head-hashes.json" \
    -o "$work_dir/affected.txt"

  affected="$(
    python3 -c 'import pathlib, sys; labels = [line.strip() for line in pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines() if line.strip()]; print(" ".join(labels))' "$work_dir/affected.txt"
  )"

  if [[ -z "$affected" ]]; then
    echo "bazel-diff produced no affected targets; falling back to //...."
    echo "fallback=true" >> "$GITHUB_OUTPUT"
    echo "affected=" >> "$GITHUB_OUTPUT"
    exit 0
  fi

  # The manual reference is removed from normal affected targets below. Capture its
  # pre-filter impact so only PRs that reach the pixel corpus run its 16 Linux shards.
  reference_targets="$(
    cd "$head_dir"
    bazelisk query --keep_going \
      'tests(//tools/ci:linux_wgpu_resvg_reference) union labels(dep, tests(//tools/ci:linux_wgpu_resvg_reference))'
  )"
  if [[ -z "$reference_targets" ]]; then
    echo "::error::Linux WebGPU resvg suite has no test target."
    exit 1
  fi
  for reference_target in $reference_targets; do
    if [[ " $affected " == *" $reference_target "* ]]; then
      echo "wgpu_reference_affected=true" >> "$GITHUB_OUTPUT"
      break
    fi
  done

  # BEGIN macos-metal selection (metal_affected).
  # The native Metal job reruns fixed suites with test caching off, so it runs only when
  # Bazel finds a test of those suites in the affected set (bazel-diff marks a test
  # affected when anything it depends on changed), or a changed path is one bazel-diff
  # cannot attribute or one the job runs outside Bazel. The dependency inputs are the
  # WebGPU reference list's (non-registry dependency definitions, the module lockfile)
  # plus every third-party BUILD overlay. It reads the selection before manual targets
  # are filtered out, like the reference above. Fail closed: any query failure, or suites
  # with no tests, selects the job.
  metal_affected=false
  while IFS= read -r path; do
    case "$path" in
      third_party/bazel/non_bcr_deps.bzl|MODULE.bazel.lock|third_party/BUILD.*|third_party/*.BUILD|\
      tools/ci_metal_toolchain.py|tools/ci_select_macos_xcode.sh|tools/ci_macos_xcode.txt|tools/ci_bazel_test.sh)
        echo "Metal job input changed: $path"
        metal_affected=true
        break
        ;;
    esac
  done <<< "$changed_files"
  if [[ "$metal_affected" != "true" ]]; then
    metal_suites='tests(//tools/ci:metal_full_validation + //tools/ci:metal_preflight + //tools/ci:metal_offline_compiler + //tools/ci:metal_native)'
    # Intentional word splitting: each affected label is one argument. Labels are quoted
    # because npm package labels contain query operators.
    # shellcheck disable=SC2086
    metal_set="$(python3 -c 'import sys; print(" ".join("\"%s\"" % label for label in sys.argv[1:]))' $affected)"
    if metal_tests="$(cd "$head_dir" && bazelisk query --keep_going "$metal_suites")" &&
        [[ -n "$metal_tests" ]] &&
        metal_targets="$(cd "$head_dir" && bazelisk query --keep_going "set($metal_set) intersect $metal_suites")"; then
      if [[ -n "$metal_targets" ]]; then
        echo "Affected Metal suite tests:"
        printf '%s\n' "$metal_targets"
        metal_affected=true
      fi
    else
      echo "Metal suite query failed or found no tests; selecting the native Metal job."
      metal_affected=true
    fi
  fi
  echo "metal_affected=$metal_affected" >> "$GITHUB_OUTPUT"
  # END macos-metal selection.

  manual_affected="$(
    # Quote each label: npm package labels contain query operators such as `+`.
    quoted_affected="$(
      python3 -c 'import pathlib, sys; print(" ".join("\"%s\"" % line.strip() for line in pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines() if line.strip()))' "$work_dir/affected.txt"
    )"
    cd "$head_dir"
    bazelisk query --keep_going "attr(\"tags\", \"manual\", set($quoted_affected))"
  )"
  if [[ -n "$manual_affected" ]]; then
    printf '%s\n' "$manual_affected" > "$work_dir/manual-affected.txt"
    affected="$(
      python3 -c 'import pathlib, sys; manual = set(pathlib.Path(sys.argv[2]).read_text(encoding="utf-8").splitlines()); labels = [line.strip() for line in pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines() if line.strip() and line.strip() not in manual]; print(" ".join(labels))' "$work_dir/affected.txt" "$work_dir/manual-affected.txt"
    )"
    echo "Filtered $(grep -c . "$work_dir/manual-affected.txt") manual-tagged affected target(s)."
  fi

  if [[ -z "$affected" ]]; then
    echo "Only manual-tagged affected targets detected; falling back to //... coverage."
    echo "fallback=true" >> "$GITHUB_OUTPUT"
    echo "affected=" >> "$GITHUB_OUTPUT"
    exit 0
  fi

  # Dependency manifests feed every browser test: add them all to the selection, so
  # each browser partition sees the changed dependency inputs. Fail closed: if they
  # cannot be listed, fall back to //....
  if [[ "$browser_inputs_changed" == "true" ]]; then
    if manifest_tests="$(
      cd "$head_dir"
      bazelisk query --keep_going \
        'attr("tags", "launches-browser", tests(//...)) except attr("tags", "manual", //...)'
    )" && [[ -n "$manifest_tests" ]]; then
      manifest_labels=()
      while IFS= read -r label; do
        if [[ -n "$label" ]]; then
          manifest_labels+=("$label")
        fi
      done <<< "$manifest_tests"
      affected="$affected ${manifest_labels[*]}"
      echo "Added browser tests for changed dependency manifests: ${manifest_labels[*]}"
    else
      echo "Could not list browser tests for changed dependency manifests; falling back to //...."
      echo "fallback=true" >> "$GITHUB_OUTPUT"
      echo "affected=" >> "$GITHUB_OUTPUT"
      exit 0
    fi
  fi

  # Trusted PRs split browser tests between the VM and native desktop. Ask
  # the PR's merge graph whether any affected test launches a browser. bazel-diff hashes
  # every transitive input in the build graph, including the Wasm package and the spec
  # files these tests read; changed dependency manifests added their tests above.
  # Fail closed: a failed query selects the browser job.
  if browser_targets="$(
    cd "$head_dir"
    bazelisk query --keep_going "attr(\"tags\", \"launches-browser\", set($affected))"
  )"; then
    if [[ -n "$browser_targets" ]]; then
      echo "Affected browser targets:"
      printf '%s\n' "$browser_targets"
      echo "browser_affected=true" >> "$GITHUB_OUTPUT"
    else
      echo "browser_affected=false" >> "$GITHUB_OUTPUT"
    fi
  else
    echo "Browser target query failed; selecting the native desktop browser job."
    echo "browser_affected=true" >> "$GITHUB_OUTPUT"
  fi

  echo "fallback=false" >> "$GITHUB_OUTPUT"
  echo "affected=$affected" >> "$GITHUB_OUTPUT"
}

select_wgpu_reference() {
  run_reference=false
  # The workflow step passes the `determine` outputs in its env; an unset one reads as empty.
  if [[ "${WGPU_REFERENCE_AFFECTED:-}" == "true" ||
        "${WGPU_REFERENCE_CHANGED:-}" == "true" ||
        "${WGPU_REFERENCE_UNKNOWN:-}" == "true" ||
        "${FULL_TEST:-}" == "true" ]]; then
    run_reference=true
  fi
  echo "run=$run_reference" >> "$GITHUB_OUTPUT"
}

usage() {
  echo "usage: tools/ci/determine_targets.sh pr-meta|determine|select-wgpu-reference" >&2
  exit 2
}

[[ $# -eq 1 ]] || usage
if [[ -z "${GITHUB_OUTPUT:-}" ]]; then
  echo "determine_targets.sh writes step outputs to the file named by GITHUB_OUTPUT; set it." >&2
  exit 2
fi
case "$1" in
  pr-meta) pr_meta ;;
  determine) determine ;;
  select-wgpu-reference) select_wgpu_reference ;;
  *) usage ;;
esac
