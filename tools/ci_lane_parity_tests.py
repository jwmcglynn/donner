"""Pins what a pull request runs against what a push to main runs.

Pull requests from the trusted operator run some lanes on self-hosted runners for speed, while
main pushes always run GitHub-hosted lanes. A pull request that is green on its lanes should not
turn main red, so every check a main push runs must also run on each kind of pull request, with
the same test selection, tag filters, test environment and Bazel flags. This test evaluates each
workflow's job and step conditions for a main push, a trusted pull request and an untrusted pull
request, then compares the Bazel invocations that can fail the run.

Every difference must be listed in an allowlist below with the reason it is acceptable. An entry
that no longer matches a difference also fails, so the lists shrink as gaps close.
"""

import copy
import functools
import hashlib
import json
from pathlib import Path
import re
import shlex
import sys
import unittest
from unittest import mock

from python.runfiles import runfiles
from tools import bazelrc_configs
from tools import ci_coverage_contract as coverage_contract
from tools.ci import lane as lane_entry
from tools.ci import lane_view
from tools.ci import lanes


_CONFIGS = []


def _repository_configs():
    """Every config in the repository's .bazelrc. A lane's command line is compared with each
    config replaced by its flags, so a lane cannot change what it runs through a config out of
    sight."""
    if not _CONFIGS:
        _CONFIGS.append(bazelrc_configs.parse(Path(runfiles.Create().Rlocation(
            "donner/.bazelrc")).read_text(encoding="utf-8")))
    return _CONFIGS[0]


def _config_flags(name, command="test"):
    """The flags a repository config applies to `command`. An allowlist entry that accepts a
    whole config, such as a main-only executor config, accepts each of its flags."""
    return bazelrc_configs.expand(command, ["--config=" + name], _repository_configs())


# The shared target selection: `//...` on a fallback, otherwise the affected labels. Steps that use
# it are compared by this symbol, since the labels are only known when the workflow runs; a lane
# phase's dry run prints it for a selection it is not given.
SELECTED_TARGETS = lane_entry.SELECTION_SYMBOL

# Workflows that run on a push to main without running on pull requests at all, or with a
# narrower pull request trigger. Keyed by (workflow, difference).
TRIGGER_ALLOWLIST = {
    ("badges.yaml", "push-only"):
        "Publishes line-count badges for main; it runs no build or test.",
    ("codeql.yml", "push-only"):
        "Static analysis of main; its findings go to code scanning and do not gate a merge.",
    ("build_docs.yaml", "push-only"):
        "Builds the documentation site from main for verified publication. Doxygen warnings "
        "are not errors, so a pull request cannot make it fail through source changes alone.",
    ("cmake.yml", "pull-request-paths"):
        "PRs that change CMake inputs can qualify the Linux compiler cache. The Linux "
        "consumer runs in main CI on PRs. Hosted macOS CMake runs only on main pushes.",
}

# Jobs that run on a main push but not on one kind of pull request, mapped to the job that runs
# in their place there. The replacement's Bazel invocations are compared step by step.
JOB_REPLACEMENTS = {
    ("main.yml", "linux", "trusted-pr"): "linux-self-hosted",
    ("lint.yml", "lint", "trusted-pr"): "lint-self-hosted",
    ("main.yml", "macos", "trusted-pr"): "macos-self-hosted",
}

# Hosted jobs that run some of a replaced job's steps on the replaced job's own runner image, for
# the pull requests that affect those steps' targets. Keyed like JOB_REPLACEMENTS; the value is
# (subset job, steps it covers). A covered step must run with exactly main's Bazel invocation, so
# it is compared with the subset job instead of the replacement. The subset runs only when the
# PR's affected set reaches the covered tests (or falls back to //...); a PR that cannot change
# them leaves main's result for them unchanged, apart from flakes main's //... run can still hit.
HOSTED_SUBSETS = {}

# Main's hosted lanes whose phases tools/ci/coverage_contract.json declares, and the phases there
# that run no Bazel command.
COVERAGE_LANES = ("linux", "macos")
COVERAGE_NON_BAZEL_PHASES = {"linux": ("Build and test CMake consumer",)}

# Main's hosted image cannot run Metal shader validation (docs/metal_validation.md).
HOSTED_NO_SHADER_VALIDATION = (
    "Main's hosted virtual GPU cannot run Metal shader validation, so its Metal tests run with "
    "the texture check off and unvalidated. Accepted risk: main tests a different GPU with "
    "weaker checks; pull request lanes on a real GPU keep full validation.")
TEXTURE_USAGE_SPLIT = (("test-env-main-only", "MTL_SHADER_VALIDATION_TEXTURE_USAGE=0"),
                       ("test-env-pr-only", "MTL_SHADER_VALIDATION_TEXTURE_USAGE=1"))
HOSTED_UNVALIDATED_CONFIG = (
    "Main's hosted virtual GPU cannot compile shader-validated Metal pipelines, so its Metal "
    "device tests request API validation only; the flag selects their test environment and no "
    "build action. Accepted risk: main tests those kernels without shader validation; the pull "
    "request lanes on a real GPU keep it.")
HOSTED_SKIPS_SHADER_VALIDATION_TESTS = (
    "Main runs fewer tests: the validation probe and the out-of-bounds canary assert shader "
    "validation, which main's hosted virtual GPU cannot run.")
# Differences between a main step and the hosted subset's copy of it. Keyed by (workflow, main
# job, subset job, step, kind, value). Only build-scope differences belong here: the subset must
# test exactly what main tests.
SUBSET_ALLOWLIST = {}

# Setup step inputs whose value in a hosted subset is fixed instead of copied from main's job. The
# subset runs only for pull requests, which must not save the Actions cache, so it states that
# directly rather than repeating main's main-only expression.
SUBSET_SETUP_REQUIRED_INPUTS = {("Setup Bazel", "cache-save"): "false"}

# Steps a hosted subset runs that main's job does not, with the reason each cannot change what
# the covered steps test.
SUBSET_EXTRA_STEPS = {
    **{step: "Records which steps ran for the coverage contract after the tests finish; it "
             "writes and uploads a receipt and changes nothing a covered step tests."
       for step in ("Record coverage receipt", "Upload coverage receipt")},
}

# Jobs that run on a main push and on no kind of pull request, without a replacement.
MAIN_ONLY_JOBS = {
    ("cmake.yml", "macos"): "Hosted macOS CMake runs only on main pushes. PRs use the Linux consumer.",
}

# Differences between a main job and its replacement. Keyed by (workflow, main job, replacement,
# step, kind, value); the reason says why the difference cannot make main red after a green PR,
# or names the accepted risk.
INVOCATION_ALLOWLIST = {
    ("lint.yml", "lint", "lint-self-hosted", "Restore Bazel", "step-main-only", ""):
        "The persistent ARM64 lint runner uses its own Bazel state, without importing hosted snapshots.",
    ("main.yml", "macos", "macos-self-hosted", "Test (browser targets)", "tag-filter-pr-only",
     "-requires_desktop_session"):
        "The native macos-browser partition runs desktop-session tests separately; "
        "the service runner cannot launch browsers that require a desktop session.",
    # The hosted image has no offline Metal compiler, so its lane drops these targets. The
    # self-hosted runner has one and runs them, so the pull request runs more than main.
    ("main.yml", "macos", "macos-self-hosted", "Test (browser targets)", "tag-filter-main-only",
     "-requires_metal_toolchain"):
        "Main runs fewer tests: the hosted image cannot compile offline Metal shaders.",
    # Browser cases quarantined on the hosted runner alone (#1691, #1702). Main skips them; the
    # pull request runs them, so they cannot turn main red. Remove with the quarantine.
    ("main.yml", "macos", "macos-self-hosted", "Build and test", "test-env-main-only",
     "DONNER_HOSTED_MACOS_QUARANTINE=1"):
        "Main skips the quarantined cases that the pull request runs (#1691, #1702).",
    ("main.yml", "macos", "macos-self-hosted", "Test (browser targets)", "test-env-main-only",
     "DONNER_HOSTED_MACOS_QUARANTINE=1"):
        "Main skips the quarantined cases that the pull request runs (#1691, #1702).",
    # Records host load and process call graphs only when a browser test reaches its deadline.
    # A persistent self-hosted runner never publishes them.
    ("main.yml", "macos", "macos-self-hosted", "Build and test", "test-env-main-only",
     "DONNER_BROWSER_STALL_DIAGNOSTICS=1"):
        "Diagnostics only: it changes what a timed-out test records, not whether it passes.",
    ("main.yml", "macos", "macos-self-hosted", "Test (browser targets)", "test-env-main-only",
     "DONNER_BROWSER_STALL_DIAGNOSTICS=1"):
        "Diagnostics only: it changes what a timed-out test records, not whether it passes.",
    # The self-hosted runner ignores its host's user and system bazelrc files.
    ("main.yml", "macos", "macos-self-hosted", "*", "startup-pr-only", "--nohome_rc"):
        "Runner-specific: keeps the host's interactive bazelrc out of CI.",
    ("main.yml", "macos", "macos-self-hosted", "*", "startup-pr-only", "--nosystem_rc"):
        "Runner-specific: keeps any host system bazelrc out of CI.",
    # Each macOS lane names itself in its test environment, so test results, which depend on
    # the runner's GPU, are never shared between the hosted and self-hosted lanes. Build action
    # keys stay identical (//tools:macos_cache_key_parity_tests).
    **{("main.yml", "macos", "macos-self-hosted", step, kind, value):
       "Lane identity for test results only; it does not change what runs."
       for step in ("Qualify pinned Chromium archive", "Build and test", "Test (browser targets)",
                    "Test (Geode editor lane)")
       for kind, value in (("test-env-main-only", "DONNER_CI_MACOS_LANE=hosted"),
                           ("test-env-pr-only", "DONNER_CI_MACOS_LANE=self-hosted-vm"))},
    **{("main.yml", "macos", "macos-self-hosted", "*", "flag-pr-only", flag):
       "VM runner scheduling budget (--config=macos-vm); it changes no action key."
       for flag in _config_flags("macos-vm")},
    **{("main.yml", "macos", "macos-self-hosted", step, "flag-pr-only",
        "--//build_defs:metal_shader_validation=off"):
       "The VM uses API validation. The independent native Metal job retains shader validation; "
       "this setting changes only Metal device tests' environment."
       for step in ("Qualify pinned Chromium archive", "Test (browser targets)",
                    "Test (Geode editor lane)")},
    # A changed non-Bazel step is keyed by a digest of both rendered definitions, so any edit to
    # what the step runs (its action, inputs, script, shell, its own env and the variables it
    # names) changes the digest and needs a new review. Overrides of the Xcode selection's test
    # hooks from job env or earlier steps are rejected by //tools:macos_cache_key_parity_tests.
    # The refresh passes the self-hosted lane's rc-skipping startup flags.
    ("main.yml", "macos", "macos-self-hosted", "Refresh Xcode toolchain configuration",
     "step-changed", "de78e4645a7c25f7"):
        "Same repositories refreshed; only the runner-specific startup flags differ.",
    # Setup the hosted runner needs and the self-hosted runner has already: the host provides
    # Bazelisk, Homebrew LLVM and the Xcode tools ("Prepare self-hosted macOS toolchain"),
    # and keeps Bazel's repository and action state between runs. The Bazel steps then fetch
    # what they need.
    **{("main.yml", "macos", "macos-self-hosted", step, "step-main-only", ""):
       "Hosted runner setup only; the self-hosted runner is provisioned with it, and it "
       "changes no test, filter or flag."
       for step in ("Install clang-tidy", "Setup Bazel", "Restore Bazel action cache")},
    ("main.yml", "macos", "macos-self-hosted", "Free hosted macOS disk", "step-main-only", ""):
        "Hosted runner setup only: it deletes the image's unused Xcodes and simulator runtimes so "
        "a cold build fits on its disk, and changes no test, filter or flag.",
    ("main.yml", "macos", "macos-self-hosted", "Save Bazel action cache", "step-main-only", ""):
        "Hosted runner teardown only: it bounds and saves the action cache that 'Restore Bazel "
        "action cache' restored, after every test step, and changes no test, filter or flag.",
}

# Differences in how a job that runs on both main and a pull request runs Bazel in each, keyed
# by (workflow, job, scenario, step, kind, value).
SHARED_JOB_ALLOWLIST = {
    ("main.yml", "linux", scenario, step, "flag-main-only", flag):
        "Cloud execution requires the trusted writer key, so other PRs run the same build and "
        "test commands, with the same declared LLVM toolchain, on the hosted runner itself. "
        "Trusted PRs use ARM64 and accept the x86_64 gap; the cloud probe remains on main."
    for scenario in ("untrusted-pr", "renovate-pr") for step in ("Build", "Test")
    for flag in _config_flags("buildbuddy-re", step.lower())
}

# The hosted writer previously built the full tree in the executor check; its Build
# now covers those roots alongside the planner selection. Read-only jobs keep their selection.
_CLOUD_TREE_REASON = "The trusted hosted Build also owns the existing full-tree cloud build."
SHARED_JOB_ALLOWLIST.update({
    ("main.yml", "linux", scenario, "Build", "target-main-only", "//..."): _CLOUD_TREE_REASON
    for scenario in ("untrusted-pr", "renovate-pr")
})

# (action, input) pairs whose value differs between main and pull requests by policy, pinned by
# the security policy tests: Actions cache saves are main-only, and BuildBuddy write access
# follows pull request trust. See _other_step_view.
_POLICY_EVENT_INPUTS = frozenset({
    ("bazel-contrib/setup-bazel", "cache-save"),
    ("./.github/actions/buildbuddy-cache", "trusted-pr"),
})

_EVENT_ROUTING_REASON = (
    "Event routing: the step decides from the event which targets, scope or runs apply. Its "
    "outputs are the scenario inputs this test models, so its own script differs by design.")

# The kinds of pull request each check is evaluated for (see SCENARIOS).
PULL_REQUEST_SCENARIOS = ("trusted-pr", "untrusted-pr", "renovate-pr")

# Non-Bazel steps of a shared job whose rendered definition differs between main and some kind
# of pull request: (workflow, job, step) to {scenario: digest of the rendered main and pull
# request definitions}. Any edit to the step, or a difference in another scenario, changes the
# observed digests, so the entry must be reviewed and updated.
_SHARED_STEP_DIGESTS = {
    ("main.yml", "determine-targets",
     "Determine affected Bazel targets"): dict.fromkeys(PULL_REQUEST_SCENARIOS, "7b92e4c03e6e4cce"),
    ("main.yml", "determine-targets",
     "Extract SHA for hash caching"): dict.fromkeys(PULL_REQUEST_SCENARIOS, "9cde4a1e41b56a52"),
}

SHARED_STEP_ALLOWLIST = {
    (workflow, job, step, scenario, digest): _EVENT_ROUTING_REASON
    for (workflow, job, step), digests in _SHARED_STEP_DIGESTS.items()
    for scenario, digest in digests.items()
}
SHARED_STEP_ALLOWLIST.update({
    ("editor_wasm.yml", "build", "Build and size-check Geode editor Wasm package", scenario,
     "217a681bde53c3dd"):
        "Only PAYLOAD_BUDGET_MODE differs: main pushes report numeric size metrics, while "
        "every pull request enforces them. The same script and functional checks run on both; "
        "this digest pins the complete rendered step and its environment in each scenario."
    for scenario in PULL_REQUEST_SCENARIOS
})

# Steps of one job that run on a main push but not on one kind of pull request.
_CACHE_WRITER_REASON = (
    "Remote cache wiring only: main and trusted PRs read and write the BuildBuddy cache, Renovate "
    "PRs read it through the read-only step, and other PRs build without it. A cache changes how "
    "fast results arrive, not which tests run or what they assert, and a colder PR build is "
    "stricter than main's.")

STEP_ALLOWLIST = {
    **{("main.yml", "linux", "Verify BuildBuddy remote execution", scenario):
       "The cold cloud-execution probe needs a trusted writer credential; trusted PRs run it."
       for scenario in ("untrusted-pr", "renovate-pr")},
    **{(workflow, job, "Configure BuildBuddy remote cache", scenario): _CACHE_WRITER_REASON
       for workflow, job in (("editor_wasm.yml", "build"), ("main.yml", "linux"),
                             ("main.yml", "macos"))
       for scenario in ("untrusted-pr", "renovate-pr")},
    # Accepted risk: the self-hosted Linux lane builds and tests on another architecture, so a
    # test failure specific to the hosted runner's architecture or speed first shows on main.
}

# Steps main runs that a kind of pull request skips, but that a different job runs on that pull
# request instead. Keyed like JOB_REPLACEMENTS; the value is (counterpart job, steps). Each step's
# Bazel invocation is compared with the counterpart's step of the same name, so a narrower target
# selection, tag filter or test environment on the pull request side fails even though the main
# step itself is in STEP_ALLOWLIST.
COUNTERPART_STEPS = {
    ("main.yml", "linux", "trusted-pr"): ("linux-self-hosted", ("Build", "Test")),
}

# Differences between a main step and its counterpart. Keyed by (workflow, main job, counterpart,
# step, kind, value).
def _counterpart_entries(main_job, counterpart, entries, reason):
    return {("main.yml", main_job, counterpart, step, kind, value): reason
            for step, kind, value in entries}


COUNTERPART_ALLOWLIST = {
    **_counterpart_entries("linux", "linux-self-hosted", [
        *[("*", "flag-main-only", flag) for flag in _config_flags("buildbuddy-re")],
    ], "Where actions run, not what they build: both lanes use the declared LLVM toolchain, and "
       "hosted Linux runs its compiles on the cloud executor while the self-hosted lane has its "
       "own executor. Tests remain on the hosted runner for its display dependencies."),
    **_counterpart_entries("linux", "linux-self-hosted", [
        ("Build", "flag-pr-only", lanes.REMOTE_ONLY),
        ("*", "flag-pr-only", "--remote_local_fallback=false"),
        ("*", "flag-pr-only", "--incompatible_remote_local_fallback_for_remote_cache=false"),
        ("*", "flag-pr-only", "--remote_timeout=60s"),
        ("*", "flag-pr-only", "--remote_download_outputs=minimal"),
        ("Test", "flag-pr-only", lanes.DIAG_TEST[-1]),
        ("Test", "startup-pr-only", lanes.REQUIRE_EXECUTOR),
        ("Test", "flag-pr-only", lanes.REMOTE_OR_DISPLAY),
        *[("Test", "flag-pr-only", flag) for flag in lanes.VENUE_ANALYSIS],
    ], "Runner-specific remote execution settings; they choose where actions run and which "
       "outputs come back, not what is built or tested."),
    **_counterpart_entries("linux", "linux-self-hosted", [
        ("Build", "flag-pr-only", "--build_event_json_file=$DIAG_DIR/build/bep.json"),
        ("Build", "flag-pr-only", "--execution_log_compact_file=$DIAG_DIR/build/exec.log.zst"),
        ("Build", "flag-pr-only", "--profile=$DIAG_DIR/build/profile.gz"),
        ("Test", "flag-pr-only", "--build_event_json_file=$DIAG_DIR/test/bep.json"),
        ("Test", "flag-pr-only", "--execution_log_compact_file=$DIAG_DIR/test/exec.log.zst"),
        ("Test", "flag-pr-only", "--profile=$DIAG_DIR/test/profile.gz"),
    ], "Diagnostics only: the self-hosted lane records profiles and logs for slow-run triage."),
    **_counterpart_entries("linux", "linux-self-hosted", [
        ("Test", "tag-filter-main-only", "-ci-remote-gpu"),
        ("Test", "tag-filter-pr-only", "-local-gpu-isolated"),
        ("Test", "target-main-only", SELECTED_TARGETS),
        ("Test", "target-pr-only",
         "$(python3 tools/ci_remote_gpu_targets.py %s)" % SELECTED_TARGETS),
    ], "The self-hosted lane swaps each locally isolated GPU suite for its remote wrapper "
       "(tools/ci_remote_gpu_targets.py), which runs the same implementation and shards, so it "
       "keeps the remote wrappers and drops the local-only targets; hosted Linux does the "
       "reverse. The two Xvfb rules select local execution in the shared test graph."),
}

_cloud_tree = _counterpart_entries("linux", "linux-self-hosted", [
    ("Build", "target-main-only", "//..."),
], _CLOUD_TREE_REASON)
COUNTERPART_ALLOWLIST.update(_cloud_tree)
INVOCATION_ALLOWLIST.update(_cloud_tree)

# The replacement still compares portable phases; x86_64 execution stays on main.
INVOCATION_ALLOWLIST.update(_counterpart_entries("linux", "linux-self-hosted", [
    *[(step, "flag-main-only", flag) for step in ("Build", "Test")
      for flag in _config_flags("buildbuddy-re", step.lower())],
    *[("*", "flag-pr-only", flag) for flag in (
        "--remote_local_fallback=false",
        "--incompatible_remote_local_fallback_for_remote_cache=false", "--remote_timeout=60s")],
    *[(step, "flag-pr-only", "--remote_download_outputs=minimal")
      for step in ("Build", "Test")],
    *[(step, "flag-pr-only", "--%s=$DIAG_DIR/%s/%s" % (flag, step.lower(), filename))
      for step in ("Build", "Test")
      for flag, filename in (("build_event_json_file", "bep.json"),
                             ("execution_log_compact_file", "exec.log.zst"),
                             ("profile", "profile.gz"))],
    ("Test", "flag-pr-only", lanes.DIAG_TEST[-1]),
    ("Test", "startup-pr-only", lanes.REQUIRE_EXECUTOR),
    ("Test", "flag-pr-only", lanes.REMOTE_OR_DISPLAY),
    ("Build", "flag-pr-only", lanes.REMOTE_ONLY),
    *[("Test", "flag-pr-only", flag) for flag in lanes.VENUE_ANALYSIS],
    ("Test deterministic fuzzer soak wrapper", "flag-pr-only", lanes.REMOTE_ONLY),
    ("Test", "tag-filter-main-only", "-ci-remote-gpu"),
    ("Test", "tag-filter-pr-only", "-local-gpu-isolated"),
    ("Test", "target-main-only", SELECTED_TARGETS),
    ("Test", "target-pr-only", "$(python3 tools/ci_remote_gpu_targets.py %s)" % SELECTED_TARGETS),
], "The ARM64 runner uses its own remote backend, strict fallback policy and diagnostics. "
   "Remote GPU wrappers retain the same implementation; native display checks run locally in the shared graph. "
   "The maintainer accepts the x86_64 toolchain/runtime gap before merge."))
INVOCATION_ALLOWLIST.update(_counterpart_entries("linux", "linux-self-hosted", [
    (step, "step-main-only", "") for step in (
        "Configure BuildBuddy remote cache", "Install system dependencies", "Setup Bazel",
        "Restore Bazel action cache", "Save Bazel action cache")
], "The persistent ARM64 runner pre-provisions its tools and uses its own remote cache; "
   "hosted setup and public cache credentials do not run there."))
INVOCATION_ALLOWLIST.update(_counterpart_entries("linux", "linux-self-hosted", [
    (step, "step-main-only", "") for step in (
        "Verify BuildBuddy remote execution", "Linker canary (non-lld linker support)")
], "Maintainer decision 2026-10-31 accepts losing hosted x86_64 PR coverage. "
   "Cloud executor and gold-linker qualification remain on hosted main."))

# Each scenario sets every job output a condition reads; an output not modelled here raises.
# `trusted_pr` and `renovate_pr` are the .github/actions/pr-trust outputs, which both the
# self-hosted routing and the remote cache wiring read; a push gets "false" for both. A PR here
# does not reach the WebGPU reference corpus (`wgpu_reference`), and it does reach a browser
# test (`browser_affected`) and a test of the native Metal suites (`metal_affected`).
_PULL_REQUEST = {
    "github.event_name": "pull_request",
    "github.ref": "refs/pull/1/merge",
    "fallback": "false",
    "affected": "//donner/editor/wasm/tests:standalone_geode_browser_renderer_test",
    "browser_affected": "true",
    "metal_affected": "true",
    "wgpu_reference": "false",
}

# Steps that run a Bazel build or test and also contain a line that can turn a failure into
# success (see failure_masks). Keyed by (workflow, job, step, line).
FAILURE_SWALLOW_ALLOWLIST = {}

SCENARIOS = {
    "main-push": {
        "github.event_name": "push",
        "github.ref": "refs/heads/main",
        "use_self_hosted": "false",
        "trusted_pr": "false",
        "renovate_pr": "false",
        "fallback": "true",
        "affected": "",
        "wgpu_reference": "",
        "browser_affected": "",
        "metal_affected": "",
    },
    "trusted-pr": dict(_PULL_REQUEST, use_self_hosted="true", trusted_pr="true",
                       renovate_pr="false"),
    # A trusted PR whose affected set reaches no browser, runner-sensitive or Metal suite test.
    "trusted-pr-native-only": dict(_PULL_REQUEST, use_self_hosted="true", trusted_pr="true",
                                   renovate_pr="false", affected="//donner/base:base_tests",
                                   browser_affected="false", metal_affected="false"),
    # A trusted PR that falls back to //... (an infrastructure change).
    "trusted-pr-fallback": dict(_PULL_REQUEST, use_self_hosted="true", trusted_pr="true",
                                renovate_pr="false", fallback="true", affected="",
                                browser_affected="", metal_affected=""),
    "untrusted-pr": dict(_PULL_REQUEST, use_self_hosted="false", trusted_pr="false",
                         renovate_pr="false"),
    "renovate-pr": dict(_PULL_REQUEST, use_self_hosted="false", trusted_pr="false",
                        renovate_pr="true"),
}


# Step outputs a condition may read, with the value used in every scenario and why. A step
# output not listed here fails the test, so a new one is reviewed before it can gate a check.
_CACHE_HIT = ("", "A cache restore result only gates that cache's save step; a miss runs every "
                  "such step.")

STEP_OUTPUTS = {
    "steps.pr-meta.outputs.identical_tree": ("", "Changed trees use the full target planner setup."),
    **{"steps.%s.outputs.cache-hit" % step_id: _CACHE_HIT
       for step_id in ("base-hashes", "bazel-cache", "ccache", "cmake-deps", "npm-cache")},
    "steps.browser_gpu_failure.outputs.run_probe": (
        "", "Gates the advisory browser GPU comparisons, which run only after a failure and "
            "cannot fail the job."),
    "steps.pr-meta.outputs.base_sha": (
        "set", "Main pushes and pull requests both have a base revision for bazel-diff."),
}

# Repository variables a condition may read, with the value used in every scenario and why. A
# variable not listed here fails the test, so a new switch is reviewed before it can gate a check.
REPOSITORY_VARIABLES = {
    "vars.UNIFIED_RE_CANARY": ("", "The default exercises the mixed-platform suite on trusted PRs."),
    "vars.MACOS_REMOTE_SHADOW_LANE": (
        "true", "Turns on the advisory macOS shadow lane for every trusted pull request. On, so "
                "the evaluation reaches that job; it never gates (continue-on-error)."),
}

# Job results and outputs of the advisory shadow lane's prerequisites, with the value used in every
# scenario and why. The shadow lane never gates, so these only decide whether it is evaluated.
SHADOW_LANE_CONTEXT = {
    "needs.linux-self-hosted.result": (
        "success", "The shadow lane runs after its run's Linux lane, passed or failed."),
    "needs.macos-remote-shadow-turnstile.outputs.admitted": (
        "true", "Its own admission let it run; otherwise it is skipped."),
}

# Job outputs a condition may read, mapped to the scenario key that holds each one's value.
_MODELLED_OUTPUTS = {
    "needs.gatekeeper.outputs.unified_re_default": "trusted_pr",
    "needs.gatekeeper.outputs.trusted_pr": "trusted_pr",
    "needs.gatekeeper.outputs.renovate_pr": "renovate_pr",
    "needs.trust.outputs.trusted_pr": "trusted_pr",
    "needs.trust.outputs.renovate_pr": "renovate_pr",
    "needs.determine-targets.outputs.fallback": "fallback",
    "needs.determine-targets.outputs.affected": "affected",
    "needs.determine-targets.outputs.wgpu_reference": "wgpu_reference",
    "needs.determine-targets.outputs.browser_affected": "browser_affected",
    "needs.determine-targets.outputs.metal_affected": "metal_affected",
}

BAZEL_COMMANDS = ("build", "test")


def _read(path):
    resolved = runfiles.Create().Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


def _workflow_paths():
    resolved = runfiles.Create().Rlocation("donner/.github/workflows/main.yml")
    directory = Path(resolved).parent
    return sorted(directory.glob("*.yml")) + sorted(directory.glob("*.yaml"))


# --- A small YAML reader for the subset GitHub workflows here use. -----------------------------


# One token of a YAML or shell line: an escaped character, a Bash ANSI-C quoted string
# (`$'...'`, where a backslash escapes the next character), a single- or double-quoted string,
# or any other character. A backslash escapes the next character except inside plain single
# quotes, as in Bash and YAML double-quoted strings.
_COMMENT_SCAN = re.compile(r"""\\.|\$'(?:\\.|[^'\\])*'?|'[^']*'?|"(?:\\.|[^"\\])*"?|.""",
                           re.DOTALL)


def _strip_comment(value, yaml=False):
    """A YAML or shell line without its trailing comment, outside quotes and escapes.

    A comment starts at a `#` token at the start of the line or after a space or tab. In shell
    text the blank must be unescaped, so `label\\ #1` keeps the `#` in the word. A YAML plain
    scalar has no escapes, so with `yaml` the `#` after an escaped blank still starts a comment.
    """
    after_blank = True
    for match in _COMMENT_SCAN.finditer(value):
        token = match.group()
        if token == "#" and after_blank:
            return value[:match.start()].rstrip()
        after_blank = (token[-1] if yaml else token) in (" ", "\t")
    return value


_CONTINUATION = re.compile(r"(\\+)\n\s*")


def _join_continuations(script):
    """A shell script with each line that ends in backslashes joined to the next with a space.

    An odd run ends in a continuation, which is removed. An even run ends in an escaped
    backslash, which is kept, so the join never turns it into an escaped blank. Joining an even
    run too keeps the text of a quote that is open across the newline on one line.
    """
    return _CONTINUATION.sub(lambda match: match.group(1)[:len(match.group(1)) // 2 * 2] + " ",
                             script)


def _scalar(value):
    value = _strip_comment(value.strip(), yaml=True)
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
        return value[1:-1]
    if value.startswith("[") and value.endswith("]"):
        return [_scalar(part) for part in value[1:-1].split(",") if part.strip()]
    return value


class _Lines:
    def __init__(self, text):
        self.lines = text.splitlines()
        self.index = 0

    def skip_blank(self):
        while self.index < len(self.lines):
            stripped = self.lines[self.index].strip()
            if stripped and not stripped.startswith("#"):
                return
            self.index += 1

    def peek_indent(self):
        self.skip_blank()
        if self.index >= len(self.lines):
            return -1
        line = self.lines[self.index]
        return len(line) - len(line.lstrip(" "))


def _block_scalar(lines, parent_indent, style):
    collected = []
    block_indent = None
    while lines.index < len(lines.lines):
        line = lines.lines[lines.index]
        if line.strip():
            indent = len(line) - len(line.lstrip(" "))
            if indent <= parent_indent:
                break
            if block_indent is None:
                block_indent = indent
            collected.append(line[block_indent:])
        else:
            collected.append("")
        lines.index += 1
    while collected and not collected[-1]:
        collected.pop()
    if style.startswith(">"):
        return " ".join(part.strip() for part in collected if part.strip())
    return "\n".join(collected) + "\n"


def _value(lines, raw, indent):
    raw = raw.strip()
    if raw in ("|", "|-", ">", ">-"):
        return _block_scalar(lines, indent, raw)
    if raw == "" or raw.startswith("#"):
        child = lines.peek_indent()
        if child > indent or (child == indent and lines.index < len(lines.lines)
                              and lines.lines[lines.index].lstrip().startswith("- ")):
            return _node(lines, child)
        return None
    return _scalar(raw)


def _sequence_item(lines, indent):
    content = lines.lines[lines.index].strip()[1:].strip()
    lines.index += 1
    if not re.match(r"^[A-Za-z0-9_.-]+:(\s|$)", content):
        return _scalar(content)
    key, _, rest = content.partition(":")
    mapping = {key: _value(lines, rest, indent + 2)}
    if lines.peek_indent() == indent + 2:
        mapping.update(_node(lines, indent + 2))
    return mapping


def _at_sequence_item(lines, indent):
    return (lines.peek_indent() == indent
            and lines.lines[lines.index].strip().startswith("-"))


def _node(lines, indent):
    lines.skip_blank()
    if _at_sequence_item(lines, indent):
        items = []
        while _at_sequence_item(lines, indent):
            items.append(_sequence_item(lines, indent))
        return items
    mapping = {}
    while lines.peek_indent() == indent:
        content = lines.lines[lines.index].strip()
        if content.startswith("- "):
            break
        match = re.match(r"^([^:]+):(\s.*|)$", content)
        if not match:
            raise ValueError("unsupported YAML line: %r" % content)
        lines.index += 1
        mapping[_scalar(match.group(1))] = _value(lines, match.group(2), indent)
    return mapping


def parse_workflow(text):
    """Parse a workflow, raising on any line the reader does not understand.

    The reader stops a mapping or sequence at the first line it cannot place, so a construct it
    does not support (a multi-line plain scalar, a flow mapping over several lines) would end the
    parse early and silently drop the rest of the file. Require every line to be consumed.
    """
    lines = _Lines(text)
    workflow = _node(lines, 0)
    lines.skip_blank()
    if lines.index < len(lines.lines):
        raise ValueError("unsupported YAML at line %d: %r"
                         % (lines.index + 1, lines.lines[lines.index]))
    return workflow


# --- Expression evaluation for `if:` conditions. -------------------------------------------------


_TOKEN = re.compile(r"\s*(?:(?P<string>'(?:[^']|'')*')|(?P<op>==|!=|&&|\|\||[!(),])|"
                    r"(?P<word>[A-Za-z_][A-Za-z0-9_.*-]*))")


class _Expression:
    def __init__(self, text, context):
        text = text.strip()
        if text.startswith("${{") and text.endswith("}}"):
            text = text[3:-2]
        self.tokens = []
        position = 0
        while position < len(text):
            if not text[position:].strip():
                break
            match = _TOKEN.match(text, position)
            if not match or match.end() == position:
                raise ValueError("cannot evaluate expression %r" % text)
            self.tokens.append(match)
            position = match.end()
        self.position = 0
        self.context = context

    def _peek(self):
        if self.position < len(self.tokens):
            token = self.tokens[self.position]
            return token.group("op") or token.group("word") or token.group("string")
        return None

    def _take(self):
        token = self.tokens[self.position]
        self.position += 1
        return token

    def evaluate(self):
        value = self._or()
        if self.position != len(self.tokens):
            raise ValueError("trailing tokens in expression")
        return value

    def _or(self):
        value = self._and()
        while self._peek() == "||":
            self._take()
            right = self._and()
            value = value if _truthy(value) else right
        return value

    def _and(self):
        value = self._compare()
        while self._peek() == "&&":
            self._take()
            right = self._compare()
            value = right if _truthy(value) else value
        return value

    def _compare(self):
        value = self._unary()
        while self._peek() in ("==", "!="):
            operator = self._take().group("op")
            right = self._unary()
            equal = str(value).lower() == str(right).lower()
            value = equal if operator == "==" else not equal
        return value

    def _unary(self):
        if self._peek() == "!":
            self._take()
            return not _truthy(self._unary())
        return self._primary()

    def _primary(self):
        token = self._take()
        if token.group("op") == "(":
            value = self._or()
            self._take()
            return value
        if token.group("string") is not None:
            return token.group("string")[1:-1].replace("''", "'")
        word = token.group("word")
        if self._peek() == "(":
            self._take()
            arguments = []
            while self._peek() != ")":
                arguments.append(self._or())
                if self._peek() == ",":
                    self._take()
            self._take()
            return self._call(word, arguments)
        if word in ("true", "false"):
            return word == "true"
        return self.context(word)

    @staticmethod
    def _call(name, arguments):
        # Conditions are evaluated for a run in which every earlier step succeeded.
        if name in ("always", "success"):
            return True
        if name in ("cancelled", "failure"):
            return False
        if name == "contains":
            haystack, needle = arguments
            if isinstance(haystack, list):
                return needle in haystack
            return str(needle).lower() in str(haystack).lower()
        raise ValueError("unsupported function %s" % name)


# A condition that only compares a matrix entry's runner OS selects the same entries on main and
# on pull requests, so such a step stays under every check. Any other use of runner.os is
# unmodelled and fails the test.
_RUNNER_OS_ONLY = re.compile(r"^(?:\$\{\{\s*)?runner\.os (?:==|!=) '[A-Za-z]+'(?:\s*\}\})?$")


def _truthy(value):
    return value not in (False, "", None, 0)


def _context(scenario):
    values = SCENARIOS[scenario]

    def lookup(name):
        if name == "steps.buildbuddy.outputs.bazel-flags":
            return "--config=buildbuddy-re" if scenario == "main-push" or values["trusted_pr"] == "true" else ""
        if name in ("github.event_name", "github.ref"):
            return values[name]
        if name.startswith("needs.gatekeeper.outputs.use_self_hosted_"):
            return values["use_self_hosted"]
        if name in ("needs.gatekeeper.outputs.is_duplicate_push",
                    "needs.gatekeeper.outputs.docs_only"):
            return "false"
        if name in _MODELLED_OUTPUTS:
            return values[_MODELLED_OUTPUTS[name]]
        if name.startswith("steps.") and name.endswith(".outcome"):
            return "success"
        if name.endswith(".labels.*.name"):
            return []
        if name in STEP_OUTPUTS:
            return STEP_OUTPUTS[name][0]
        if name in REPOSITORY_VARIABLES:
            return REPOSITORY_VARIABLES[name][0]
        if name in SHADOW_LANE_CONTEXT:
            return SHADOW_LANE_CONTEXT[name][0]
        # An output that is not modelled above can decide which checks run, so it fails the
        # test until it is reviewed and listed.
        raise ValueError("unmodelled context %s" % name)

    return lookup


def runs(condition, scenario):
    if condition is None:
        return True
    if isinstance(condition, bool):
        return condition
    if _RUNNER_OS_ONLY.match(str(condition).strip()):
        return True
    return _truthy(_Expression(str(condition), _context(scenario)).evaluate())


# --- Bazel invocation extraction. ----------------------------------------------------------------


def _expand(text, environment):
    def replace(match):
        name = match.group(1) or match.group(2)
        return environment.get(name, match.group(0))

    return re.sub(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)", replace, text)


def _split_words(text):
    try:
        return shlex.split(text, comments=True)
    except ValueError:
        return text.split()


# A shell assignment, quoted or bare, not part of a longer word or an option.
_ASSIGNMENT = re.compile(r'(?<![\w=-])([A-Za-z_][A-Za-z0-9_]*)=("(?:[^"\\]|\\.)*"|\S+)')
_WHOLE_VARIABLE = re.compile(r"^\$(?:\{([A-Za-z_]\w*)\}|([A-Za-z_]\w*))$")


def _resolve(value, environment, variables):
    """A script variable's value with the variables it reads substituted, as one string."""
    if len(value) >= 2 and value[0] == value[-1] == '"':
        value = value[1:-1]
    return _expand(value, {**environment, **variables})


def _expand_word(word, environment, variables):
    """Expand one shell word. A script variable stays one word; an env variable may split."""
    match = _WHOLE_VARIABLE.match(word)
    if match:
        name = match.group(1) or match.group(2)
        if name in variables:
            return [variables[name]]
        if name in environment:
            return environment[name].split()
    return [_expand(word, environment)]


def _invocation(line, environment, variables):
    """One `bazelisk build` or `bazelisk test` command line as option sets, or None."""
    match = re.search(r"(?:^|\s)(bazelisk|bazel)\s(.*)$", line)
    if not match:
        return None
    words = [expanded for word in _split_words(match.group(2))
             for expanded in _expand_word(word, environment, variables)]
    startup = []
    while words and words[0].startswith("--"):
        startup.append(words.pop(0))
    if not words or words[0] not in BAZEL_COMMANDS:
        return None
    command = words.pop(0)
    # The whole command line, so the spaced `--config name` form expands too.
    words = bazelrc_configs.expand(command, words, _repository_configs())
    options = [word for word in words if word.startswith("--")]
    return {
        "command": command,
        "wrapped": "tools/ci_bazel_test.sh" in line[: match.start(1)],
        "startup": set(startup),
        "tag_filters": _tag_filters(options),
        "test_env": {option.split("=", 1)[1] for option in options
                     if option.startswith("--test_env=")},
        "flags": {option for option in options
                  if not option.startswith(("--test_env=", "--test_tag_filters="))},
        "targets": {word for word in words if not word.startswith("-")},
    }


_TEMPLATE = re.compile(r"\$\{\{(.*?)\}\}", re.DOTALL)


def render(text, scenario):
    """Evaluate each `${{ }}` expression in `text` for a scenario.

    An expression that reads a context the scenarios do not model (secrets, runner paths, the
    commit) is left as written, so it compares equal across scenarios.
    """
    def replace(match):
        try:
            value = _Expression(match.group(1), _context(scenario)).evaluate()
        except (ValueError, IndexError):
            return match.group(0)
        if isinstance(value, bool):
            return "true" if value else "false"
        return str(value)

    return _TEMPLATE.sub(replace, text)


def bazel_invocations(script, environment, scenario=None):
    """Every `bazelisk build` or `bazelisk test` command in a step script, as option sets.

    Script variables assigned before a command are resolved, so two steps that select targets
    differently compare as different. With a scenario, the script's `${{ }}` expressions are
    evaluated for it.
    """
    if scenario is not None:
        script = render(script, scenario)
    script = _join_continuations(script)
    variables = {}
    invocations = []
    for line in script.splitlines():
        # A commented-out command runs nothing.
        line = _strip_comment(line)
        invocation = _invocation(line, environment, variables)
        if invocation:
            invocations.append(invocation)
            continue
        for name, value in _ASSIGNMENT.findall(line):
            variables[name] = _resolve(value, environment, variables)
    return invocations


# A step that runs one phase of the lane table (tools/ci/lanes.py).
_LANE_CALL = re.compile(r"^python3 tools/ci/lane\.py phase ([\w-]+) ([\w-]+)$")
# The job environment that carries the planner's selection to a lane phase. A phase is compared
# with the selection left out, so its dry run prints SELECTED_TARGETS in its place;
# test_lane_steps_pass_the_planner_selection pins what the jobs pass.
SELECTION_ENVIRONMENT = ("TARGETS_FALLBACK", "TARGETS_AFFECTED")


def lane_phase(step):
    """(lane, phase) of a step that runs a lane phase, else None."""
    match = _LANE_CALL.match(str(step.get("run") or "").strip())
    return match.groups() if match else None


def lane_description(step, environment=None):
    """A lane step's dry run: its actions with the step's environment and the selection symbol."""
    environment = {key: value for key, value in (environment or {}).items()
                   if key not in SELECTION_ENVIRONMENT}
    return lane_entry.dry_run(*lane_phase(step), environment)


def lane_commands(step, environment=None):
    """The argv of each Bazel command (and query) a lane step runs, in order."""
    return [action["argv"] for action in lane_description(step, environment)["actions"]
            if action["action"] in ("bazel", "query")]


def step_invocations(step, environment, scenario=None):
    """Every Bazel build or test a step runs, as option sets.

    A lane step's commands come from its phase's dry run with the step's environment; any other
    step's from its script.
    """
    if lane_phase(step):
        return [invocation for argv in lane_commands(step, environment)
                for invocation in [_invocation(shlex.join(argv), {}, {})] if invocation]
    return bazel_invocations(str(step.get("run") or ""), environment, scenario)


# Every lane step's dry run in every scenario, reviewed: a change to the lane table, or to what a
# job passes a phase, changes it. Each command is also recorded with the repository's configs
# expanded, which a change that only moves flags into a config leaves as it is. Regenerate with
# `python3 tools/ci_lane_parity_tests.py --update-lane-golden` and review the diff.
LANE_GOLDEN = "tools/ci/lane_commands.golden.json"


def _golden_action(action):
    action = dict(action)
    if action["action"] in ("bazel", "query"):
        # So a change that only moves flags into a .bazelrc config shows as no change here.
        action["expanded"] = shlex.join(lane_view.expanded(action["argv"],
                                                           _repository_configs()))
    if "argv" in action:
        action["argv"] = shlex.join(action["argv"])
    return action


def lane_steps(workflow):
    """(job name, job, step) for every step that runs a lane phase."""
    return [(job_name, job, step) for job_name, job in _jobs(workflow).items()
            for step in job.get("steps") or [] if lane_phase(step)]


def _golden_entry(workflow, job_name, job, step):
    groups = {}
    for scenario in SCENARIOS:
        actions = [_golden_action(action) for action in lane_description(
            step, _step_environment(workflow, job, step, scenario))["actions"]]
        groups.setdefault(json.dumps(actions, sort_keys=True), []).append(scenario)
    return {"step": _step_name(step),
            "runs_in": [scenario for scenario in SCENARIOS
                        if job_runs(workflow, job_name, scenario)
                        and runs(step.get("if"), scenario)],
            "actions": [{"scenarios": scenarios, "actions": json.loads(actions)}
                        for actions, scenarios in groups.items()]}


def lane_golden(workflow):
    """Each lane step's actions per scenario, grouped by the scenarios that agree, and the
    scenarios in which the step runs."""
    return {"%s %s" % lane_phase(step): _golden_entry(workflow, job_name, job, step)
            for job_name, job, step in lane_steps(workflow)}


def _tag_filters(options):
    filters = [option.split("=", 1)[1] for option in options
               if option.startswith("--test_tag_filters=")]
    if not filters:
        return None
    return {tag for tag in filters[-1].split(",") if tag}


def runtime_flag_writes(script):
    """`NAME=value` for each lane flag variable (`*_FLAGS`) a step rewrites through GITHUB_ENV.

    The value is the literal text up to the closing quote or end of line, so a step that writes
    different flags compares as different. A value built at run time (a Python writer joining
    strings) leaves only its literal prefix, which is empty.
    """
    if "GITHUB_ENV" not in script:
        return set()
    # Not `\b`: a Python writer spells the line break before the name as a literal "\n".
    return {"%s=%s" % (name, value.strip()) for name, value in
            re.findall(r"(?<![A-Z0-9_])([A-Z][A-Z0-9_]*_FLAGS)=([^\"\n]*)", script)}


# --- The comparison. -----------------------------------------------------------------------------


def _jobs(workflow):
    return workflow.get("jobs") or {}


def _environment(workflow, job):
    environment = dict(workflow.get("env") or {})
    environment.update(job.get("env") or {})
    return {key: str(value) for key, value in environment.items()}


def _continues_on_error(entry, scenario):
    """Whether a job's or step's `continue-on-error` is set for this scenario."""
    value = entry.get("continue-on-error")
    if value is None:
        return False
    if str(value).lower() in ("true", "false"):
        return str(value).lower() == "true"
    return runs(value, scenario)


# A status check function in a job's `if:` replaces GitHub's implicit `success()`, so the job can
# run even when a job it needs was skipped.
_STATUS_FUNCTION = re.compile(r"\b(?:always|failure|cancelled|success)\s*\(")


def _needs(job):
    needs = job.get("needs") or []
    return [needs] if isinstance(needs, str) else list(needs)


def job_runs(workflow, job_name, scenario):
    """Whether a job runs in this scenario, including the jobs it needs.

    Without a status check function in its `if:`, GitHub skips a job when any job it needs was
    skipped (failures are not modelled: every scenario assumes earlier jobs succeed).
    """
    job = _jobs(workflow)[job_name]
    condition = str(job.get("if") or "")
    if not runs(job.get("if"), scenario):
        return False
    if _STATUS_FUNCTION.search(condition) and "success(" not in condition:
        return True
    return all(job_runs(workflow, need, scenario) for need in _needs(job))


def job_gates(workflow, job_name, scenario):
    """Whether a job runs in this scenario and its failure fails the run."""
    return (job_runs(workflow, job_name, scenario)
            and not _continues_on_error(_jobs(workflow)[job_name], scenario))


def _gating_steps(job, scenario):
    """Steps that check something and can fail the job in this scenario.

    Under a job-level `continue-on-error`, no step can fail the run. A step-level one exempts
    that step. A cache save checks nothing: it records state for later runs, and only main
    pushes may write caches.
    """
    if _continues_on_error(job, scenario):
        return
    for step in job.get("steps") or []:
        if _continues_on_error(step, scenario):
            continue
        if str(step.get("uses") or "").startswith("actions/cache/save@"):
            continue
        yield step


def _step_name(step):
    """A step's name or id; otherwise its action and inputs, or the first line of its script."""
    if step.get("name") or step.get("id"):
        return step.get("name") or step.get("id")
    if step.get("uses"):
        return "%s %s" % (step["uses"], sorted((step.get("with") or {}).items()))
    return "run: " + str(step.get("run") or "").strip().split("\n", 1)[0]


def _step_environment(workflow, job, step, scenario=None):
    """The environment a step's script sees: workflow, then job, then step `env:`.

    With a scenario, `${{ }}` expressions in the values are evaluated for it.
    """
    environment = _environment(workflow, job)
    environment.update({key: str(value) for key, value in (step.get("env") or {}).items()})
    if scenario is not None:
        environment = {key: render(value, scenario) for key, value in environment.items()}
    return environment


def _bazel_profile(workflow, job, scenario):
    """The Bazel invocations of each gating step that runs, and the flag variables rewritten."""
    steps = {}
    writes = set()
    for step in _gating_steps(job, scenario):
        if not runs(step.get("if"), scenario):
            continue
        writes |= runtime_flag_writes(str(step.get("run") or ""))
        invocations = step_invocations(step, _step_environment(workflow, job, step, scenario),
                                       scenario)
        if invocations:
            steps[_step_name(step)] = invocations
    return steps, writes


# GitHub's default environment variables that a script moved out of a workflow step reads in
# place of the step's expressions. They render like the expressions they replaced.
_DEFAULT_ENVIRONMENT_CONTEXTS = {"GITHUB_EVENT_NAME": "github.event_name",
                                 "GITHUB_SHA": "github.sha"}


@functools.cache
def _workflow_logic_scripts():
    """The scripts that hold workflow step logic, as the target selection lists them."""
    function = _read("tools/ci/determine_targets.sh").split("is_workflow_logic() {\n", 1)[1]
    function = function.split("\n  }\n", 1)[0]
    return frozenset(path for arm in re.findall(r"^\s+([\w./|-]+)\)$", function, re.MULTILINE)
                     for path in arm.split("|"))


def _step_script(run):
    """A step's script followed by each workflow logic script it runs.

    The step is compared with the logic it moved into a script, so an edit there needs the same
    review as an edit to the step, and the event routing it reads from the default environment
    still renders per scenario.
    """
    script = run
    # A lane step is compared with what its phase runs.
    if lane_phase({"run": run}):
        script += "\n# %s\n%s" % (run.strip(), json.dumps(lane_description({"run": run}),
                                                          sort_keys=True))
    for path in sorted(set(re.findall(r"(?<![\w/.-])tools/ci/[\w.-]+\.sh\b", run))
                       & _workflow_logic_scripts()):
        body = _read(path)
        for variable, context in _DEFAULT_ENVIRONMENT_CONTEXTS.items():
            body = re.sub(r"\$\{%s\}|\$%s\b" % (variable, variable),
                          lambda _, context=context: "${{ %s }}" % context, body)
        script += "\n# %s\n%s" % (path, body)
    return script


def _other_step_view(workflow, job, step, scenario):
    """What a non-Bazel step runs in a scenario: action, inputs, command, shell, variables read.

    `${{ }}` expressions are rendered for the scenario, so a check that is stricter on one event
    than another compares as different. Each variable the command or inputs read is recorded
    with its effective value, wherever it is set.
    """
    script = render(_step_script(str(step.get("run") or "")), scenario)
    environment = _step_environment(workflow, job, step, scenario)
    # Inputs that differ between events by policy, for the action that reads them, are not
    # compared (_POLICY_EVENT_INPUTS).
    action = str(step.get("uses") or "").split("@", 1)[0]
    inputs = sorted((key, render(str(value), scenario))
                    for key, value in (step.get("with") or {}).items()
                    if (action, key) not in _POLICY_EVENT_INPUTS)
    # A step's own `env:` exists only for that step, so every key counts as read, even one the
    # command reads indirectly (for example a script it runs).
    read = (set(re.findall(r"\$\{?([A-Za-z_][A-Za-z0-9_]*)", script))
            | set(re.findall(r"\benv\.([A-Za-z_][A-Za-z0-9_]*)", str(inputs)))
            | set(step.get("env") or {}))
    return (step.get("uses"), inputs, script, step.get("shell"),
            sorted((name, environment.get(name)) for name in read))


def _other_steps(workflow, job, scenario):
    """Gating steps that run in this scenario and run no Bazel build or test, by name."""
    other = {}
    for step in _gating_steps(job, scenario):
        if runs(step.get("if"), scenario) and not step_invocations(
                step, _step_environment(workflow, job, step)):
            other[_step_name(step)] = _other_step_view(workflow, job, step, scenario)
    return other


def _other_step_differences(workflow, main_job, pr_job):
    """Non-Bazel checks main's job runs that the pull request job lacks or runs differently."""
    main_other = _other_steps(workflow, main_job, "main-push")
    pr_other = _other_steps(workflow, pr_job, "trusted-pr")
    return ({(name, "step-main-only", "") for name in set(main_other) - set(pr_other)}
            | {(name, "step-changed", definition_digest(main_other[name], pr_other[name]))
               for name in set(main_other) & set(pr_other)
               if main_other[name] != pr_other[name]})


def _set_differences(name, kind, main_values, pr_values):
    return ({(name, kind + "-main-only", value) for value in main_values - pr_values}
            | {(name, kind + "-pr-only", value) for value in pr_values - main_values})


def _run_differences(name, main_run, pr_run):
    differences = set()
    if main_run["wrapped"] != pr_run["wrapped"]:
        differences.add((name, "exit-status-wrapper", ""))
    if (main_run["tag_filters"] is None) != (pr_run["tag_filters"] is None):
        differences.add((name, "tag-filter-presence", ""))
    for kind, key in (("startup", "startup"), ("flag", "flags"), ("test-env", "test_env"),
                      ("tag-filter", "tag_filters"), ("target", "targets")):
        differences |= _set_differences(name, kind, main_run[key] or set(),
                                        pr_run[key] or set())
    return differences


def _step_differences(name, main_runs, pr_runs):
    if [run["command"] for run in main_runs] != [run["command"] for run in pr_runs]:
        return {(name, "commands", "")}
    differences = set()
    for main_run, pr_run in zip(main_runs, pr_runs):
        differences |= _run_differences(name, main_run, pr_run)
    return differences


def _compared_steps(main_steps, pr_steps, only_steps):
    """Differences in just `only_steps`, each of which both jobs must run."""
    differences = {(name, "step-missing", "") for name in only_steps
                   if name not in main_steps or name not in pr_steps}
    shared = {name for name in only_steps if name in main_steps and name in pr_steps}
    for name in shared:
        differences |= _step_differences(name, main_steps[name], pr_steps[name])
    return _fold_shared(differences, shared)


def invocation_differences(workflow, main_job_name, replacement_name, only_steps=None):
    """(step, kind, value) differences between a main job and the job that replaces it.

    With `only_steps`, compare just those steps, each of which both jobs must run.
    """
    jobs = _jobs(workflow)
    main_steps, main_writes = _bazel_profile(workflow, jobs[main_job_name], "main-push")
    pr_steps, pr_writes = _bazel_profile(workflow, jobs[replacement_name], "trusted-pr")
    if only_steps is not None:
        return _compared_steps(main_steps, pr_steps, only_steps)
    differences = _set_differences("*", "runtime-env", main_writes, pr_writes)
    differences |= {(name, "step-main-only", "") for name in set(main_steps) - set(pr_steps)}
    differences |= _other_step_differences(workflow, jobs[main_job_name], jobs[replacement_name])
    shared = set(main_steps) & set(pr_steps)
    for name in shared:
        differences |= _step_differences(name, main_steps[name], pr_steps[name])
    return _fold_shared(differences, shared)


def _coverage_phases(workflow, lane):
    """The gating checks main's hosted lane runs on a push: its Bazel steps and listed others."""
    job = _jobs(workflow)[lane]
    phases = {_step_name(step) for step in _gating_steps(job, "main-push")
              if runs(step.get("if"), "main-push") and step_invocations(
                  step, _step_environment(workflow, job, step))}
    return phases | set(COVERAGE_NON_BAZEL_PHASES.get(lane, ()))


def _setup_view(step):
    """A setup step's identity: condition, what it runs, and its inputs as a sorted list."""
    inputs = dict(step.get("with") or {})
    for step_name, key in SUBSET_SETUP_REQUIRED_INPUTS:
        if _step_name(step) == step_name:
            inputs.pop(key, None)
    return (step.get("if"), step.get("uses"), step.get("run"), sorted(inputs.items()))


def _required_input_problems(name, step):
    return [(name, "%s is not %s" % (key, value))
            for (step_name, key), value in SUBSET_SETUP_REQUIRED_INPUTS.items()
            if step_name == name and str((step.get("with") or {}).get(key)) != value]


def subset_setup_differences(workflow, main_job_name, subset_name, covered):
    """Setup steps that main's job runs before a covered step and the subset lacks or changes.

    Setup is every gating step before the first covered step that runs no Bazel build or test:
    checkout, toolchain installs, cache restores and dependency fetches. Bazel builds and tests
    there are the work the subset deliberately skips, and its Bazel steps are compared
    separately.
    """
    jobs = _jobs(workflow)
    main_job, subset = jobs[main_job_name], jobs[subset_name]
    environment = _environment(workflow, main_job)
    subset_steps = {_step_name(step): step for step in _gating_steps(subset, "trusted-pr")}
    # The job-level environment binding decides which cache key the setup steps receive.
    problems = ([] if subset.get("environment") == main_job.get("environment")
                else [("environment", "changed")])
    for step in _gating_steps(main_job, "main-push"):
        name = _step_name(step)
        if name in covered:
            break
        if step_invocations(step, environment):
            continue
        if name not in subset_steps:
            problems.append((name, "missing"))
            continue
        if _setup_view(step) != _setup_view(subset_steps[name]):
            problems.append((name, "changed"))
        problems.extend(_required_input_problems(name, subset_steps[name]))
    return problems + subset_order_problems(main_job, subset)


def subset_order_problems(main_job, subset):
    """Subset steps that main's job does not run, or runs in a different order.

    The subset's gating steps, in order, must be a subsequence of main's: its setup steps (so
    BuildBuddy and toolchain setup still precede every Bazel step), the Bazel steps it keeps, the
    covered steps and any later failure handling. A step main's job does not have could change
    what the covered steps test (for example, by writing a bazelrc), so it needs a reviewed
    entry in SUBSET_EXTRA_STEPS.
    """
    main_names = [_step_name(step) for step in _gating_steps(main_job, "main-push")]
    problems = []
    position = -1
    for step in _gating_steps(subset, "trusted-pr"):
        name = _step_name(step)
        if name not in main_names:
            if name not in SUBSET_EXTRA_STEPS:
                problems.append((name, "not in main's job"))
            continue
        index = main_names.index(name)
        if index <= position:
            problems.append((name, "out of order"))
        position = max(position, index)
    return problems


def subset_differences(workflow, main_job_name, subset_name, covered):
    """How a hosted subset job's Bazel invocations differ from the main job's.

    Every Bazel step the subset runs must be a main step with the same invocation, every covered
    step must be among them, and both jobs must rewrite the same flag variables at run time.
    """
    jobs = _jobs(workflow)
    main_steps, main_writes = _bazel_profile(workflow, jobs[main_job_name], "main-push")
    subset_steps, subset_writes = _bazel_profile(workflow, jobs[subset_name], "trusted-pr")
    differences = _set_differences("*", "runtime-env", main_writes, subset_writes)
    differences |= {(name, "step-missing", "") for name in set(covered) - set(subset_steps)}
    differences |= {(name, "step-subset-only", "") for name in set(subset_steps) - set(main_steps)}
    for name in set(subset_steps) & set(main_steps):
        differences |= _step_differences(name, main_steps[name], subset_steps[name])
    return differences


def _fold_shared(differences, shared_steps):
    """Report a flag difference once under "*" when every shared step has it."""
    folded = set()
    by_value = {}
    for step, kind, value in differences:
        by_value.setdefault((kind, value), set()).add(step)
    for (kind, value), steps in by_value.items():
        if kind.startswith(("startup-", "flag-")) and shared_steps and steps >= shared_steps:
            folded.add(("*", kind, value))
        else:
            folded.update((step, kind, value) for step in steps)
    return folded


def _branch_pattern(pattern):
    """A GitHub branch filter pattern (`*`, `**`, `?`) as an anchored regular expression."""
    parts = re.split(r"(\*\*|\*|\?)", pattern)
    wildcards = {"**": ".*", "*": "[^/]*", "?": "."}
    return re.compile("".join(wildcards.get(part, re.escape(part)) for part in parts) + r"\Z")


def branch_filter_includes(branch, branches=None, ignore=None):
    """Whether a `branches` or `branches-ignore` filter lets an event on `branch` run.

    `branches` patterns apply in order, and a later `!pattern` excludes what an earlier pattern
    included; any matching `branches-ignore` pattern excludes the branch.
    """
    if ignore is not None:
        return not any(_branch_pattern(pattern).match(branch) for pattern in ignore)
    if branches is None:
        return True
    included = False
    for pattern in branches:
        negated = pattern.startswith("!")
        if _branch_pattern(pattern[1:] if negated else pattern).match(branch):
            included = not negated
    return included


def triggers(trigger):
    """A workflow's `on:` as a mapping, expanding `on: push` and `on: [push, pull_request]`."""
    if isinstance(trigger, str):
        return {trigger: None}
    if isinstance(trigger, list):
        return {event: None for event in trigger}
    return trigger if isinstance(trigger, dict) else {}


# Pull request activity types that cover every new revision of an open pull request.
_REVISION_TYPES = {"opened", "synchronize", "reopened"}


def _push_runs_on_main(trigger):
    trigger = triggers(trigger)
    if "push" not in trigger:
        return False
    push = trigger["push"] or {}
    # A push trigger that filters only tags never runs for branch pushes.
    if push.get("branches") is None and push.get("branches-ignore") is None and (
            "tags" in push or "tags-ignore" in push):
        return False
    return branch_filter_includes("main", push.get("branches"), push.get("branches-ignore"))


def _pull_request_trigger_differences(name, trigger):
    trigger = triggers(trigger)
    if "pull_request" not in trigger:
        return {(name, "push-only")}
    observed = set()
    push_paths = (trigger["push"] or {}).get("paths")
    pull_request = trigger["pull_request"] or {}
    pr_paths = pull_request.get("paths")
    if pr_paths is not None and pr_paths != push_paths:
        observed.add((name, "pull-request-paths"))
    if not branch_filter_includes("main", pull_request.get("branches"),
                                  pull_request.get("branches-ignore")):
        observed.add((name, "pull-request-branches"))
    if pull_request.get("paths-ignore") is not None:
        observed.add((name, "pull-request-paths-ignore"))
    types = pull_request.get("types")
    if types is not None and not _REVISION_TYPES <= set(types):
        observed.add((name, "pull-request-types"))
    return observed


def trigger_differences(workflows, main_push_workflows):
    """How each main-push workflow's pull request trigger is narrower than its push trigger."""
    observed = set()
    for name in main_push_workflows:
        observed |= _pull_request_trigger_differences(name, workflows[name]["on"])
    return observed


def _event_starts(workflow, scenario):
    """Whether a workflow's trigger starts a run for this scenario's event. A pull request's
    path filter is taken to match: a pull request can change any path."""
    trigger = triggers(workflow.get("on"))
    values = SCENARIOS[scenario]
    if values["github.event_name"] == "pull_request":
        return "pull_request" in trigger
    if "push" not in trigger:
        return False
    push = trigger["push"] or {}
    branch = values["github.ref"].removeprefix("refs/heads/")
    return branch_filter_includes(branch, push.get("branches"), push.get("branches-ignore"))


# The CMake generator check, run directly or through the CMake consumer script.
_GENERATOR_CHECK = re.compile(
    r"gen_cmakelists\.py\b[^\n]*--check\b|cmake_consumer\.sh build\b[^\n]*--check\b")


def _runs_generator_check(workflow, job_name, scenario):
    checks = [step for step in _jobs(workflow)[job_name].get("steps") or []
              if _GENERATOR_CHECK.search(str(step.get("run") or ""))]
    return bool(checks) and job_runs(workflow, job_name, scenario) and any(
        runs(step.get("if"), scenario) for step in checks)


def generator_check_jobs(workflows, scenario):
    """(workflow, job) pairs that run the CMake generator check for this scenario's event."""
    return {(name, job_name) for name, workflow in sorted(workflows.items())
            if _event_starts(workflow, scenario)
            for job_name in _jobs(workflow)
            if _runs_generator_check(workflow, job_name, scenario)}


def _jobs_on_both_triggers(workflows, main_push_workflows):
    for name in main_push_workflows:
        workflow = workflows[name]
        if "pull_request" in triggers(workflow.get("on")):
            for job_name, job in _jobs(workflow).items():
                yield name, job_name, job


def skipped_main_jobs(workflows, main_push_workflows):
    """Jobs main runs that no pull request runs, and (job, scenario) pairs one kind skips."""
    main_only = set()
    skipped = set()
    for name, job_name, job in _jobs_on_both_triggers(workflows, main_push_workflows):
        workflow = workflows[name]
        if not job_gates(workflow, job_name, "main-push"):
            continue
        missing = [scenario for scenario in PULL_REQUEST_SCENARIOS
                   if not job_gates(workflow, job_name, scenario)]
        if len(missing) == len(PULL_REQUEST_SCENARIOS):
            main_only.add((name, job_name))
        else:
            skipped.update((name, job_name, scenario) for scenario in missing)
    return main_only, skipped


def _running_steps(job, scenario):
    """Names of the gating steps that run in this scenario."""
    return {_step_name(step) for step in _gating_steps(job, scenario)
            if runs(step.get("if"), scenario)}


def _scenario_differences(workflow, job, scenario):
    """How one shared job's rendered steps differ between a main push and a scenario."""
    main_steps, _ = _bazel_profile(workflow, job, "main-push")
    pr_steps, _ = _bazel_profile(workflow, job, scenario)
    # A step that runs in both scenarios but runs Bazel only on main (its command rendered away
    # on pull requests). Steps the pull request skips outright are reported by
    # skipped_main_steps.
    differences = {(step, "step-main-only", "") for step in set(main_steps) - set(pr_steps)
                   if step in _running_steps(job, scenario)}
    for step in set(main_steps) & set(pr_steps):
        differences |= _step_differences(step, main_steps[step], pr_steps[step])
    main_other = _other_steps(workflow, job, "main-push")
    pr_other = _other_steps(workflow, job, scenario)
    return differences | {(step, "step-changed", definition_digest(main_other[step], pr_other[step]))
                          for step in set(main_other) & set(pr_other)
                          if main_other[step] != pr_other[step]}


def _kinds(differences):
    """(step, kind) of each difference, for tests that do not pin a definition digest."""
    return {(step, kind) for step, kind, _ in differences}


def definition_digest(main_view, pr_view):
    """A stable digest of a step's rendered definitions on main and on a pull request."""
    text = json.dumps([main_view, pr_view], sort_keys=True)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


def shared_job_differences(workflows, main_push_workflows):
    """How a job that runs on both main and a pull request runs differently on each.

    A shared step's script, inputs or environment can branch on the event through `${{ }}`
    expressions, so each scenario's rendered Bazel invocations and other checks are compared
    step by step.
    """
    observed = set()
    for name, job_name, job in _jobs_on_both_triggers(workflows, main_push_workflows):
        workflow = workflows[name]
        if not job_gates(workflow, job_name, "main-push"):
            continue
        for scenario in PULL_REQUEST_SCENARIOS:
            if job_gates(workflow, job_name, scenario):
                observed.update((name, job_name, scenario) + difference for difference in
                                _scenario_differences(workflow, job, scenario))
    return observed


def skipped_main_steps(workflows, main_push_workflows):
    """Gating steps main runs that a pull request skips while running the same job."""
    observed = set()
    for name, job_name, job in _jobs_on_both_triggers(workflows, main_push_workflows):
        if not job_gates(workflows[name], job_name, "main-push"):
            continue
        main_steps = _running_steps(job, "main-push")
        for scenario in PULL_REQUEST_SCENARIOS:
            if job_gates(workflows[name], job_name, scenario):
                observed.update((name, job_name, step, scenario)
                                for step in main_steps - _running_steps(job, scenario))
    return observed


# A Bazel build or test command, after any startup options.
_BAZEL_COMMAND = r"(?:bazelisk|bazel)\s+(?:--\S+\s+)*(?:build|test)\b"
_BAZEL_RUN = re.compile(r"(?:^|[\s;&|(!])" + _BAZEL_COMMAND)
# Pattern-based checks of a Bazel-running step's script for constructs that can turn a failure
# into success. Workflow authors are trusted: these guard against accidental drift and are not
# exhaustive.
_NEGATED_BAZEL = re.compile(r"!\s*[^;&|]*?(?:^|[\s/\"'])" + _BAZEL_COMMAND)
# A Bazel command used as the condition of `if`, `elif`, `while` or `until`, which exempts it from
# errexit.
_CONDITION_BAZEL = re.compile(r"\b(?:if|elif|while|until)\s+(?:!\s*)?(?:[^;]*?[\s/\"'])?"
                              + _BAZEL_COMMAND)
_EXIT = re.compile(r"(?<![\w$.-])exit\b(?![\w=-])\s*([^\s;&|)]*)")
_SET = re.compile(r"(?:^|[;&|]\s*|\b(?:then|do|else)\s+)set(?:\s+([^;&|]*))?")
_SET_OPTION = re.compile(r"^[-+][A-Za-z]+$")
_SET_OPTION_NAMES = {"allexport", "braceexpand", "emacs", "errexit", "errtrace", "functrace",
                     "hashall", "histexpand", "history", "ignoreeof", "keyword", "monitor",
                     "noclobber", "noexec", "noglob", "nolog", "notify", "nounset", "onecmd",
                     "physical", "pipefail", "posix", "privileged", "verbose", "vi", "xtrace"}


def _set_option_masks(word):
    """Whether one `set` word is not a literal option group, or turns errexit off (`+e`)."""
    return not _SET_OPTION.match(word) or (word.startswith("+") and "e" in word[1:])


def _set_masks(arguments):
    """Whether a `set` command's arguments are other than literal options, or disable errexit.

    An option group ending in `o` (`-o`, `-euo`, `+o`) takes the next word as an option name.
    """
    words = arguments.split()
    index = 0
    while index < len(words):
        word = words[index]
        if _set_option_masks(word):
            return True
        if word.endswith("o"):
            index += 1
            name = words[index] if index < len(words) else ""
            if name not in _SET_OPTION_NAMES or (word.startswith("+") and name == "errexit"):
                return True
        index += 1
    return False


def _line_masks(line):
    """Whether one logical line can turn a failure into success."""
    bazel = _BAZEL_RUN.search(line)
    if bazel and re.search(r"&&|\|\|", line[bazel.start():]):
        return True
    if _NEGATED_BAZEL.search(line) or _CONDITION_BAZEL.search(line):
        return True
    if re.search(r"\|\|\s*(?:true|:)\s*$", line):
        return True
    for match in _EXIT.finditer(line):
        if not re.fullmatch(r"[1-9][0-9]*", match.group(1)):
            return True
    if any(_set_masks(match.group(1) or "") for match in _SET.finditer(line)):
        return True
    return bool(re.search(r"\btrap\b", line) and re.search(r"(?<![\w$.-])exit\b", line))


def _close_group(open_groups, rest):
    """Close the innermost group; whether it held a Bazel command and `rest` uses && or ||."""
    contained = open_groups.pop()
    if contained and open_groups:
        open_groups[-1] = True
    return bool(contained and re.search(r"^[^(]*?(?:&&|\|\|)", rest))


def _scan_groups(line, open_groups):
    """Track `(` groups across lines; whether this line closes a Bazel group into && or ||."""
    bazel = _BAZEL_RUN.search(line)
    bazel_at = bazel.start() if bazel else -1
    masked = False
    for index, char in enumerate(line):
        if char == "(":
            open_groups.append(False)
        elif char == ")" and open_groups:
            masked = _close_group(open_groups, line[index + 1:]) or masked
        if index == bazel_at:
            open_groups[:] = [True] * len(open_groups)
    return masked


def _group_masks(lines):
    """Lines that close a `(` or `$(` group containing a Bazel command, then use && or ||."""
    open_groups = []
    return {line for line in lines if _scan_groups(line, open_groups)}


def failure_masks(script, shell=None):
    """Each line of a Bazel-running step that can mask a failure, after joining continuations.

    Pattern-based; it guards against accidental drift and is not exhaustive. It flags a `shell:`
    other than unset or `bash`; `!` before a Bazel command; `&&` or `||` after a Bazel command,
    or after a group that contains one; any `exit` whose argument is not a literal non-zero
    integer; any `set` whose arguments are not all literal options, or that disables errexit;
    a `trap` that runs `exit`; and any line ending in `|| true` or `|| :`.
    """
    found = set()
    if shell not in (None, "bash"):
        found.add("shell: %s" % shell)
    # A `${{ }}` expression is evaluated by GitHub before the shell runs, so its operators are
    # not shell operators.
    script = _TEMPLATE.sub("EXPR", script)
    lines = [" ".join(_strip_comment(line).split())
             for line in _join_continuations(script).splitlines()]
    lines = [line for line in lines if line]
    found.update(line for line in lines if _line_masks(line))
    return found | _group_masks(lines)


def failure_swallowing_lines(workflows, names):
    """(workflow, job, step, line) for each Bazel-running gating step line that can mask failure.

    A Bazel command compared by this test proves nothing if its step then ignores the exit
    status, so such lines must be reviewed and allowlisted.
    """
    found = set()
    for name, job_name, job in _jobs_on_both_triggers(workflows, names):
        for step in _bazel_running_steps(workflows[name], job):
            found.update((name, job_name, _step_name(step), line)
                         for line in failure_masks(str(step["run"]), step.get("shell")))
    return found


def _bazel_running_steps(workflow, job):
    """Gating steps that run a Bazel build or test in any scenario."""
    seen = {}
    for scenario in ("main-push",) + PULL_REQUEST_SCENARIOS:
        for step in _gating_steps(job, scenario):
            if runs(step.get("if"), scenario) and step_invocations(
                    step, _step_environment(workflow, job, step)):
                seen[id(step)] = step
    return list(seen.values())


# `--config=ci` writes local build event files with plain file paths, so losing the remote cache
# cannot fail a Bazel command whose build and tests passed (see .bazelrc).
BUILD_EVENT_FILE_FORMATS = ("json", "binary", "text")
_BUILD_EVENT_FILE = re.compile(r"--build_event_(?:%s)_file=" % "|".join(BUILD_EVENT_FILE_FORMATS))
# Any spelling that turns path conversion back on, including the old `experimental_` names.
_PATH_CONVERSION_ON = re.compile(
    r"--(?:experimental_)?build_event_\w+_file_path_conversion(?:=(?:true|yes|1))?$",
    re.IGNORECASE)


def _build_event_flag_problems(flags):
    """Why one Bazel command's build event file could depend on the remote cache."""
    if not any(_BUILD_EVENT_FILE.match(flag) for flag in flags):
        return []
    problems = [flag for flag in flags if _PATH_CONVERSION_ON.match(flag)]
    # `--config=ci` turns path conversion off for every format; its flags are expanded here.
    for kind in BUILD_EVENT_FILE_FORMATS:
        if any(flag.startswith("--build_event_%s_file=" % kind) for flag in flags) and (
                "--nobuild_event_%s_file_path_conversion" % kind not in flags):
            problems.append("writes a %s build event file with path conversion on" % kind)
    return problems


def _bazel_commands(workflows):
    """(workflow, job, step, invocation) for each Bazel command in any step, whatever its `if:`."""
    for name, workflow in workflows.items():
        for job_name, job in _jobs(workflow).items():
            for step in job.get("steps") or []:
                for invocation in step_invocations(step, _step_environment(workflow, job, step)):
                    yield name, job_name, _step_name(step), invocation


# The editor build identity every non-release CI lane builds with (see `ci-test-lane` in
# .bazelrc). A fixed commit keeps the editor binaries, its Wasm package and the tests built on them
# cacheable across commits; release, BCR preflight and the Editor WASM package pass the real one.
_RELEASE_COMMIT_FLAG = "--//build_defs:release_source_commit="
TEST_LANE_EDITOR_COMMIT = _RELEASE_COMMIT_FLAG + "0" * 40
# Workflows and scripts that build an artifact which ships or qualifies a release.
RELEASE_IDENTITY_SOURCES = (
    ".github/workflows/bcr_preflight.yml", ".github/workflows/editor_wasm.yml",
    ".github/workflows/publish_bcr.yml", ".github/workflows/release.yml",
    ".github/workflows/release_candidate.yml", ".github/workflows/sign_macos_release.yml",
    "tools/bcr_preflight.py", "tools/ci/editor_wasm_lane.sh", "tools/ci/editor_wasm_stage.py",
    "tools/release_candidate.py", "tools/run-browser-ci.sh",
)
_CI_CONFIG = re.compile(r"--config[= ]ci(?![\w-])")


def editor_identity_problems(workflows):
    """main.yml Bazel builds and tests that would not build the test-lane editor identity."""
    problems = []
    for _, job, step, invocation in _bazel_commands({"main.yml": workflows["main.yml"]}):
        if invocation["command"] not in ("build", "test", "coverage"):
            continue
        commits = sorted(flag for flag in invocation["flags"]
                         if flag.startswith(_RELEASE_COMMIT_FLAG))
        if commits != [TEST_LANE_EDITOR_COMMIT]:
            problems.append("%s %s: editor identity %s" % (job, step, commits))
    return sorted(set(problems))


def build_event_file_problems(workflows):
    """Bazel commands whose build event file would depend on the remote cache."""
    return sorted({"%s %s %s: %s" % (name, job, step, problem)
                   for name, job, step, invocation in _bazel_commands(workflows)
                   for problem in _build_event_flag_problems(invocation["flags"])})


class CiLaneParityTest(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.workflows = {}
        for path in _workflow_paths():
            cls.workflows[path.name] = parse_workflow(path.read_text(encoding="utf-8"))
        cls.main_push_workflows = sorted(
            name for name, workflow in cls.workflows.items()
            if _push_runs_on_main(workflow.get("on")))

    def test_reader_understands_the_main_workflow(self):
        jobs = _jobs(self.workflows["main.yml"])
        for name in ("gatekeeper", "determine-targets", "linux", "linux-self-hosted", "macos",
                     "macos-self-hosted"):
            self.assertIn(name, jobs)
        steps = [_step_name(step) for step in jobs["macos"]["steps"]]
        self.assertIn("Build and test", steps)
        self.assertIn("Test (browser targets)", steps)
        test = next(step for step in jobs["macos"]["steps"] if _step_name(step) == "Build and test")
        self.assertEqual(lane_phase(test), ("macos", "test"))
        [command] = lane_commands(test)
        self.assertEqual(command[:3], ["tools/ci_bazel_test.sh", "bazelisk", "test"])
        self.assertIn("--config=hosted-macos-quarantine", command)
        self.assertIn("main.yml", self.main_push_workflows)

    def test_expressions_follow_the_routing_gate(self):
        jobs = _jobs(self.workflows["main.yml"])
        expected = {
            "macos": {"main-push": True, "trusted-pr": False, "untrusted-pr": True},
            "macos-self-hosted": {"main-push": False, "trusted-pr": True, "untrusted-pr": False},
            "macos-metal": {"main-push": False, "trusted-pr": True, "untrusted-pr": False},
            "linux": {"main-push": True, "trusted-pr": False, "untrusted-pr": True},
            "linux-self-hosted": {"main-push": False, "trusted-pr": True, "untrusted-pr": False},
            "macos-remote-shadow": {"main-push": False, "trusted-pr": True,
                                    "untrusted-pr": False},
        }
        for job, scenarios in expected.items():
            for scenario, expected_runs in scenarios.items():
                with self.subTest(job=job, scenario=scenario):
                    self.assertEqual(runs(jobs[job].get("if"), scenario), expected_runs)
        self.assertFalse(runs("${{ !cancelled() && failure() }}", "main-push"))
        self.assertTrue(runs("${{ github.event_name == 'push' || false }}", "main-push"))
        with self.assertRaises(ValueError):
            runs("${{ github.unknown_context == 'x' }}", "main-push")
        # A step gated only on a matrix entry's OS stays under every check in every scenario;
        # combining the OS with anything else could hide a main-only step, so it is unmodelled.
        for scenario in ("main-push", "trusted-pr"):
            for condition in ("runner.os == 'macOS'", "${{ runner.os != 'Linux' }}"):
                with self.subTest(scenario=scenario, condition=condition):
                    self.assertTrue(runs(condition, scenario))
            for condition in ("runner.os != 'macOS' || github.event_name == 'push'",
                              "!(runner.os == 'macOS')", "contains(runner.os, 'mac')"):
                with self.subTest(scenario=scenario, condition=condition):
                    with self.assertRaisesRegex(ValueError, "unmodelled context runner.os"):
                        runs(condition, scenario)

    def test_advisory_shadow_lane_never_gates(self):
        workflow = self.workflows["main.yml"]
        for scenario in SCENARIOS:
            with self.subTest(scenario=scenario):
                self.assertFalse(job_gates(workflow, "macos-remote-shadow", scenario))
        self.assertTrue(job_runs(workflow, "macos-remote-shadow", "trusted-pr"))
        job = dict(_jobs(workflow)["macos-remote-shadow"])
        job.pop("continue-on-error")
        self.assertTrue(job_gates(dict(workflow, jobs=dict(_jobs(workflow), **{
            "macos-remote-shadow": job})), "macos-remote-shadow", "trusted-pr"))

    def test_every_main_push_workflow_runs_on_pull_requests(self):
        self._assert_matches_allowlist(
            trigger_differences(self.workflows, self.main_push_workflows), TRIGGER_ALLOWLIST,
            "main runs these workflows but pull requests do not; run them on pull requests "
            "with the same filters or allowlist them with a reason")

    def _assert_matches_allowlist(self, observed, allowlist, message):
        self.assertEqual(sorted(observed - set(allowlist)), [], message)
        self.assertEqual(sorted(set(allowlist) - observed), [], "stale allowlist entries")

    def test_every_job_a_main_push_runs_also_runs_on_pull_requests(self):
        main_only, skipped = skipped_main_jobs(self.workflows, self.main_push_workflows)
        self._assert_matches_allowlist(main_only, MAIN_ONLY_JOBS,
                                       "jobs main runs that no pull request runs")
        self._assert_matches_allowlist(
            skipped, JOB_REPLACEMENTS,
            "jobs main runs that a kind of pull request skips without a replacement")
        for (name, job_name, scenario), replacement in JOB_REPLACEMENTS.items():
            self.assertTrue(job_gates(self.workflows[name], replacement, scenario),
                            "%s does not gate a %s" % (replacement, scenario))

    def test_every_step_a_main_push_runs_also_runs_on_pull_requests(self):
        self._assert_matches_allowlist(
            skipped_main_steps(self.workflows, self.main_push_workflows), STEP_ALLOWLIST,
            "steps main runs that a pull request skips in the same job")

    def test_replacement_jobs_run_the_same_bazel_invocations(self):
        observed = set()
        # A step a hosted subset covers is still compared with the replacement's: the
        # replacement keeps running it on every trusted PR, including those the subset skips.
        for (name, job_name, _), replacement in JOB_REPLACEMENTS.items():
            workflow = self.workflows[name]
            for step, kind, value in invocation_differences(workflow, job_name, replacement):
                observed.add((name, job_name, replacement, step, kind, value))
        self._assert_matches_allowlist(
            observed, INVOCATION_ALLOWLIST,
            "the pull request lane runs Bazel differently from main's lane")

    def test_counterpart_steps_run_the_same_bazel_invocations(self):
        observed = set()
        for (name, job_name, scenario), (counterpart, steps) in COUNTERPART_STEPS.items():
            with self.subTest(counterpart=counterpart):
                for step in steps:
                    # A counterpart stands in for a main step the pull request skips.
                    self.assertTrue(
                        (name, job_name, step, scenario) in STEP_ALLOWLIST or
                        JOB_REPLACEMENTS.get((name, job_name, scenario)) == counterpart)
                self.assertTrue(job_gates(self.workflows[name], counterpart, scenario))
            workflow = self.workflows[name]
            for step, kind, value in invocation_differences(workflow, job_name, counterpart,
                                                            only_steps=steps):
                observed.add((name, job_name, counterpart, step, kind, value))
        self._assert_matches_allowlist(
            observed, COUNTERPART_ALLOWLIST,
            "the pull request runs a skipped main step differently in its counterpart job")

    def test_target_selection_differences_are_reported(self):
        # A lane phase that narrows the planner's selection differs from main's phase.
        workflow = self.workflows["main.yml"]
        self.assertFalse({difference for difference in
                          invocation_differences(workflow, "macos", "macos-self-hosted")
                          if difference[1].startswith("target-")})
        narrowed = _replacing(lanes.SELECTED, "//donner/base/...")
        with _with_phase_args("macos-self-hosted", "test", narrowed):
            differences = invocation_differences(workflow, "macos", "macos-self-hosted")
        self.assertLessEqual({("Build and test", "target-main-only", SELECTED_TARGETS),
                              ("Build and test", "target-pr-only", "//donner/base/...")}, differences)
        # A step script's targets, through its environment, compare the same way.
        main = textwrap_job('X_TARGETS: "//tools/ci:a"', "bazelisk test --config=ci $X_TARGETS")
        narrowed = textwrap_job('X_TARGETS: "//tools/ci:b"',
                                "bazelisk test --config=ci //donner/base/... $X_TARGETS")
        workflow = parse_workflow("jobs:\n  main:\n" + main + "  pr:\n" + narrowed)
        self.assertEqual(invocation_differences(workflow, "main", "pr"), {
            ("Test", "target-pr-only", "//donner/base/..."),
            ("Test", "target-main-only", "//tools/ci:a"),
            ("Test", "target-pr-only", "//tools/ci:b"),
        })
        workflow = parse_workflow("jobs:\n  main:\n" + main + "  pr:\n" + main)
        self.assertEqual(invocation_differences(workflow, "main", "pr"), set())

    def test_lane_steps_match_the_reviewed_golden(self):
        observed = lane_golden(self.workflows["main.yml"])
        golden = json.loads(_read(LANE_GOLDEN))
        self.assertEqual(sorted(golden), sorted(observed))
        for key in observed:
            with self.subTest(phase=key):
                self.assertEqual(golden[key], observed[key],
                                 "regenerate %s with --update-lane-golden" % LANE_GOLDEN)

    def test_every_lane_config_is_used_by_the_lane_table(self):
        # A `lane-*` config exists only for the lane table, so removing a lane or phase cannot
        # leave its config behind, and the table names no lane config the .bazelrc lacks.
        defined = {name for name in _repository_configs() if name.startswith("lane-")}
        named = {word.split("=", 1)[1] for lane_name, entry in lanes.LANES.items()
                 for phase in entry.phases for action in lane_entry.dry_run(
                     lane_name, phase, {})["actions"]
                 for word in action.get("argv", ()) if word.startswith("--config=lane-")}
        self.assertTrue(defined)
        self.assertEqual(defined, named)

    def test_every_lane_phase_runs_in_one_step_of_its_job(self):
        # A phase runs in the job of its lane's name, in the step its table entry names; a step
        # with an id runs the phase of that id.
        jobs = _jobs(self.workflows["main.yml"])
        called = {}
        for job_name, job in jobs.items():
            for step in job.get("steps") or []:
                phase = lane_phase(step)
                if not phase:
                    self.assertNotIn("tools/ci/lane.py", str(step.get("run") or ""))
                    continue
                with self.subTest(job=job_name, step=_step_name(step)):
                    self.assertEqual(phase[0], job_name)
                    self.assertNotIn(phase, called)
                    called[phase] = step
                    self.assertEqual(lanes.LANES[phase[0]].phases[phase[1]].step,
                                     _step_name(step))
                    if step.get("id"):
                        self.assertEqual(step["id"], phase[1])
        self.assertEqual(sorted(called), sorted(
            (name, phase) for name, lane in lanes.LANES.items() for phase in lane.phases))

    def test_lane_steps_pass_the_planner_selection(self):
        # A phase reads the selection only from its job's environment, set from the planner's
        # outputs, so the dry run's symbol stands for exactly the planner's selection.
        expected = {"TARGETS_FALLBACK": "${{ needs.determine-targets.outputs.fallback }}",
                    "TARGETS_AFFECTED": "${{ needs.determine-targets.outputs.affected }}"}
        workflow = self.workflows["main.yml"]
        reading = {job_name for job_name, _, step in lane_steps(workflow)
                   if SELECTED_TARGETS in json.dumps(lane_description(step))}
        for job_name, job in _jobs(workflow).items():
            uses = job_name in reading
            with self.subTest(job=job_name):
                passed = {key: value for key, value in (job.get("env") or {}).items()
                          if key in SELECTION_ENVIRONMENT}
                self.assertEqual(passed, expected if uses else {})
                self.assertTrue(not uses or "determine-targets" in _needs(job))
                self.assertEqual([], [step for step in job.get("steps") or []
                                      if set(step.get("env") or {}) & set(SELECTION_ENVIRONMENT)])
        for scenario, values in SCENARIOS.items():
            for name, key in (("TARGETS_FALLBACK", "fallback"), ("TARGETS_AFFECTED", "affected")):
                self.assertEqual(render(expected[name], scenario), values[key])

    def test_deferred_lane_failures_are_advisory(self):
        # A phase that runs past a failed command (a diagnostic comparison) still exits with that
        # failure; only a step that cannot fail its job may run one.
        jobs = _jobs(self.workflows["main.yml"])
        deferring = set()
        for job_name, job in jobs.items():
            for step in job.get("steps") or []:
                if lane_phase(step) and any(action.get("defer") for action in
                                            lane_description(step)["actions"]):
                    deferring.add((job_name, _step_name(step)))
                    self.assertEqual(str(step.get("continue-on-error")), "true", step)
        self.assertEqual(deferring, {("macos", "Compare browser GPU tests (serial)"),
                                     ("macos", "Compare browser GPU tests (parallel)")})

    def test_reader_rejects_what_it_cannot_parse(self):
        with self.assertRaisesRegex(ValueError, "unsupported YAML"):
            parse_workflow("jobs:\n  a:\n    env:\n      X: one\n        two\n"
                           "  b:\n    runs-on: x\n")
        self.assertEqual(parse_workflow("a:\n  b: c\n# trailing comment\n\n"),
                         {"a": {"b": "c"}})

    def test_job_level_continue_on_error_makes_a_job_non_gating(self):
        job = {"if": "${{ github.event_name == 'pull_request' }}", "continue-on-error": "true",
               "steps": [{"name": "Test", "run": "bazelisk test //..."}]}
        self.assertFalse(job_gates({"jobs": {"x": job}}, "x", "trusted-pr"))
        self.assertEqual(list(_gating_steps(job, "trusted-pr")), [])
        job["continue-on-error"] = "${{ github.event_name == 'push' }}"
        self.assertTrue(job_gates({"jobs": {"x": job}}, "x", "trusted-pr"))
        self.assertEqual(len(list(_gating_steps(job, "trusted-pr"))), 1)

    def test_bazel_steps_do_not_swallow_failure(self):
        self._assert_matches_allowlist(
            failure_swallowing_lines(self.workflows, self.main_push_workflows),
            FAILURE_SWALLOW_ALLOWLIST,
            "a step that runs Bazel can turn its failure into success")

    def test_build_event_files_do_not_depend_on_the_remote_cache(self):
        rc_lines = set(Path(runfiles.Create().Rlocation("donner/.bazelrc"))
                       .read_text(encoding="utf-8").splitlines())
        for kind in BUILD_EVENT_FILE_FORMATS:
            line = "common:ci --nobuild_event_%s_file_path_conversion" % kind
            self.assertTrue(line in rc_lines, ".bazelrc lacks: " + line)
        # A later config on the same command line would override `common:ci`.
        self.assertEqual([], [line for line in rc_lines if not line.startswith("#") and any(
            _PATH_CONVERSION_ON.match(word) for word in line.split())])
        self.assertEqual([], build_event_file_problems(self.workflows))

    def test_ci_lanes_build_one_constant_editor_identity(self):
        commands = [invocation for _, _, _, invocation
                    in _bazel_commands({"main.yml": self.workflows["main.yml"]})
                    if invocation["command"] in ("build", "test", "coverage")]
        self.assertGreater(len(commands), 20)
        self.assertEqual([], editor_identity_problems(self.workflows))
        # It comes from the one named config, which only `ci` applies.
        rc = [line for line in _read(".bazelrc").splitlines() if not line.startswith("#")]
        self.assertEqual([line for line in rc if "ci-test-lane" in line], [
            "common:ci-test-lane " + TEST_LANE_EDITOR_COMMIT,
            "common:ci --config=ci-test-lane",
        ])
        self.assertEqual([line for line in rc if _RELEASE_COMMIT_FLAG in line],
                         ["common:ci-test-lane " + TEST_LANE_EDITOR_COMMIT])
        # A lane phase that leaves `ci` out, or overrides the commit, is found.
        for edit in (_replacing("--config=ci"),
                     _replacing("--config=geode", "--config=geode",
                                _RELEASE_COMMIT_FLAG + "1" * 40)):
            with _with_phase_args("macos", "geode_test", edit):
                self.assertTrue(editor_identity_problems(self.workflows))

    def test_release_builds_keep_their_real_editor_identity(self):
        for path in RELEASE_IDENTITY_SOURCES:
            with self.subTest(path=path):
                self.assertNotRegex(_read(path), _CI_CONFIG)
        # Each of these passes the commit it builds explicitly.
        self.assertIn('source_args=("--//build_defs:release_source_commit=${GITHUB_SHA}")',
                      _read("tools/ci/editor_wasm_lane.sh"))
        self.assertIn('return ["--//build_defs:release_source_commit=" + commit]',
                      _read("tools/ci/editor_wasm_stage.py"))
        self.assertIn('f"--//build_defs:release_source_commit={commit}"',
                      _read("tools/bcr_preflight.py"))
        self.assertRegex("bazelisk test --config=ci //x", _CI_CONFIG)
        self.assertRegex("bazelisk test --config ci //x", _CI_CONFIG)
        self.assertNotRegex("bazelisk test --config=ci-macos //x", _CI_CONFIG)

    def test_build_event_file_problems_are_found(self):
        geode = "Test (Geode editor lane)"
        for edit, problem in (
                (_replacing("--config=ci"), "json build event file with path conversion on"),
                (_replacing("--config=ci", "--config=ci",
                            "--build_event_json_file_path_conversion"),
                 "--build_event_json_file_path_conversion"),
                (_replacing("--config=ci", "--config=ci",
                            "--experimental_build_event_json_file_path_conversion=True"),
                 "--experimental_build_event_json_file_path_conversion=True")):
            with _with_phase_args("macos", "geode_test", edit):
                problems = build_event_file_problems({"main.yml": self.workflows["main.yml"]})
            self.assertTrue(any(geode in entry and problem in entry for entry in problems),
                            (problem, problems))

    def test_failure_swallowing_lines_are_found(self):
        workflow = parse_workflow(
            "on:\n  push:\n  pull_request:\n"
            "jobs:\n  build:\n    steps:\n"
            "      - name: Test\n        run: |\n          set +e\n"
            "          bazelisk test //...\n          exit 0\n"
            "      - name: Fetch\n        run: |\n          bazelisk fetch //... || true\n")
        self.assertEqual(failure_swallowing_lines({"x.yml": workflow}, ["x.yml"]), {
            ("x.yml", "build", "Test", "set +e"),
            ("x.yml", "build", "Test", "exit 0"),
        })
        masking = (
            "bazelisk test //... || exit 0",
            "tools/ci_bazel_test.sh bazelisk --nohome_rc build --config=ci \\\n  //a:b \\\n"
            "  || echo skipped",
            "set +o errexit",
            "set +eu",
            "set -u +e",
            "set -o pipefail +e",
            "  exit 0  # done",
            "cleanup || true",
            "cleanup || :",
            "bazelisk test //... && echo passed",
            "if ! bazelisk test //...; then echo failed; fi",
            "! tools/ci_bazel_test.sh bazelisk test //...",
            "exit $((0))",
            "exit \"$x\"",
            "exit",
            "set $relax",
            "set -o \"$mode\"",
            "trap 'exit 0' ERR",
            "if bazelisk test //...; then echo passed; fi",
            "elif tools/ci_bazel_test.sh bazelisk test //a; then echo passed",
            "while bazel build //a; do break; done",
            "until bazelisk --nohome_rc test //a; do echo retry; done",
        )
        for script in masking:
            with self.subTest(script=script):
                self.assertEqual(len(failure_masks(script)), 1, script)
        # A group holding a Bazel command, closed and then followed by || or &&.
        self.assertEqual(failure_masks("out=$(\n  bazelisk test //...\n) || echo failed"),
                         {") || echo failed"})
        self.assertEqual(failure_masks("(\n  cd x\n  bazelisk build //a\n) && echo built"),
                         {") && echo built"})
        self.assertEqual(failure_masks("bazelisk test //...", "bash --noprofile --norc {0}"),
                         {"shell: bash --noprofile --norc {0}"})
        for script in ("set -euo pipefail", "set +x", "set -x", "bazelisk fetch //a:b || retry",
                       "bazelisk test ${{ github.event_name == 'push' && '//...' || '//a' }}",
                       "[[ -n \"$a\" || -n \"$b\" ]]", "exit 1", "exit 4  # mapped",
                       "[[ -n \"$a\" ]] && bazelisk test //a", "trap cleanup EXIT",
                       "# exit status is mapped below", "x=$(python3 a.py) || exit 1"):
            with self.subTest(script=script):
                self.assertEqual(failure_masks(script), set(), script)
        for shell in (None, "bash"):
            self.assertEqual(failure_masks("bazelisk test //...", shell), set())

    def test_step_names_are_unique_within_each_job(self):
        # Steps are compared by name, so a second step with a compared step's name could hide
        # a main-only check behind the step that matches.
        for name, workflow in self.workflows.items():
            for job_name, job in _jobs(workflow).items():
                names = [_step_name(step) for step in job.get("steps") or []]
                duplicates = sorted({step for step in names if names.count(step) > 1})
                self.assertEqual(duplicates, [], "%s:%s" % (name, job_name))

    def test_every_modelled_step_output_is_read(self):
        read = set()
        for name in self.main_push_workflows:
            for job in _jobs(self.workflows[name]).values():
                for entry in [job] + list(job.get("steps") or []):
                    for key in ("if", "continue-on-error"):
                        read.update(re.findall(r"steps\.[A-Za-z0-9_-]+\.outputs\.[A-Za-z0-9_-]+",
                                               str(entry.get(key) or "")))
        self.assertEqual(sorted(set(STEP_OUTPUTS) - read), [], "stale STEP_OUTPUTS entries")

    def test_step_environment_overrides_the_job_environment(self):
        main = textwrap_job('X_FLAGS: "--test_env=A=1"', "bazelisk test $X_FLAGS //...")
        overridden = textwrap_job('X_FLAGS: "--test_env=A=1"', "bazelisk test $X_FLAGS //...")
        overridden = overridden.replace(
            "        run: |", "        env:\n          X_FLAGS: --test_filter=NoSuchCase\n"
            "        run: |")
        workflow = parse_workflow("jobs:\n  main:\n" + main + "  pr:\n" + overridden)
        self.assertEqual(invocation_differences(workflow, "main", "pr"), {
            ("Test", "test-env-main-only", "A=1"),
            ("*", "flag-pr-only", "--test_filter=NoSuchCase"),
        })

    def test_runtime_rewrites_compare_their_values(self):
        self.assertEqual(runtime_flag_writes(
            'echo "BAZEL_X_FLAGS=--a --b" >> "$GITHUB_ENV"\n'), {"BAZEL_X_FLAGS=--a --b"})
        self.assertEqual(runtime_flag_writes('echo "BAZEL_X_FLAGS=--a"\n'), set())
        self.assertEqual(runtime_flag_writes(
            'output.write("\\nBAZEL_X_FLAGS=" + flags)  # GITHUB_ENV\n'), {"BAZEL_X_FLAGS="})
        # Every lane flag variable counts, not only BAZEL_*.
        self.assertEqual(runtime_flag_writes(
            'echo "HOSTED_MACOS_QUARANTINE_TEST_FLAGS=" >> "$GITHUB_ENV"\n'
            'echo "BROWSER_STALL_TEST_FLAGS=--test_env=X=1" >> "$GITHUB_ENV"\n'),
            {"HOSTED_MACOS_QUARANTINE_TEST_FLAGS=", "BROWSER_STALL_TEST_FLAGS=--test_env=X=1"})

    def test_unmodelled_job_outputs_raise(self):
        with self.assertRaisesRegex(ValueError, "unmodelled context"):
            runs("${{ needs.gatekeeper.outputs.some_new_output == 'true' }}", "trusted-pr")
        with self.assertRaisesRegex(ValueError, "unmodelled context"):
            runs("${{ steps.test.outputs.main_only == 'true' }}", "main-push")
        self.assertFalse(runs("${{ steps.base-hashes.outputs.cache-hit == 'true' }}",
                              "main-push"))
        self.assertTrue(runs("${{ steps.pr-meta.outputs.base_sha != '' }}", "trusted-pr"))
        self.assertEqual(runs("${{ needs.gatekeeper.outputs.trusted_pr == 'true' }}",
                              "trusted-pr"), True)
        self.assertEqual(runs("${{ needs.trust.outputs.renovate_pr == 'true' }}",
                              "renovate-pr"), True)
        self.assertEqual(runs("${{ needs.gatekeeper.outputs.trusted_pr == 'true' }}",
                              "main-push"), False)

    def test_unified_re_canary_default_and_rollback(self):
        job = _jobs(self.workflows["main.yml"])["linux-self-hosted"]
        step = next(step for step in job["steps"]
                    if step.get("name") == "Test Linux and macOS with native platform transitions")
        label = "ci:native-platform-canary"
        for event, base_default, labels, switch, expected in (
                ("pull_request", "true", [], "", True),
                ("pull_request", "", [], "", False),
                ("pull_request", "", [label], "", True),
                ("pull_request", "true", [], "false", False),
                ("pull_request", "", [label], "false", False),
                ("push", "true", [label], "", False)):
            with self.subTest(event=event, base=base_default, labels=labels, switch=switch):
                context = {"github.event_name": event, "vars.UNIFIED_RE_CANARY": switch,
                           "needs.gatekeeper.outputs.unified_re_default": base_default,
                           "github.event.pull_request.labels.*.name": labels}
                self.assertEqual(_Expression(step["if"], context.__getitem__).evaluate(), expected)

    def test_pull_request_branch_filters_must_include_main(self):
        workflows = {
            "release.yml": parse_workflow("on:\n  push:\n    branches: [main]\n"
                                          "  pull_request:\n    branches: [release]\n"),
            "all.yml": parse_workflow("on:\n  push:\n    branches: [main]\n"
                                      "  pull_request:\n    branches: [main, release]\n"),
        }
        self.assertEqual(trigger_differences(workflows, sorted(workflows)),
                         {("release.yml", "pull-request-branches")})

    def test_jobs_skip_when_a_needed_job_is_skipped(self):
        workflow = parse_workflow(
            "jobs:\n  prepare:\n    if: github.event_name == 'push'\n"
            "  check:\n    needs: prepare\n"
            "  report:\n    needs: [prepare]\n    if: ${{ !cancelled() }}\n"
            "  strict:\n    needs: prepare\n    if: ${{ success() && true }}\n")
        self.assertTrue(job_gates(workflow, "check", "main-push"))
        self.assertFalse(job_gates(workflow, "check", "trusted-pr"))
        self.assertTrue(job_gates(workflow, "report", "trusted-pr"))
        self.assertFalse(job_gates(workflow, "strict", "trusted-pr"))
        self.assertEqual(skipped_main_jobs({"x.yml": dict(workflow, on={"push": None,
                                                                         "pull_request": None})},
                                           ["x.yml"])[0],
                         {("x.yml", "prepare"), ("x.yml", "check"), ("x.yml", "strict")})

    def test_replacements_compare_non_bazel_checks(self):
        main = ("    steps:\n      - name: Lint\n        run: python3 tools/lint.py\n"
                "      - name: Test\n        run: bazelisk test //...\n")
        workflow = parse_workflow(
            "jobs:\n  main:\n" + main + "  pr:\n    steps:\n"
            "      - name: Test\n        run: bazelisk test //...\n")
        self.assertEqual(invocation_differences(workflow, "main", "pr"),
                         {("Lint", "step-main-only", "")})
        changed = parse_workflow(
            "jobs:\n  main:\n" + main + "  pr:\n" + main.replace("lint.py", "lint.py --fast"))
        self.assertEqual(_kinds(invocation_differences(changed, "main", "pr")),
                         {("Lint", "step-changed")})

    def test_shared_jobs_run_bazel_alike_on_main_and_pull_requests(self):
        observed = shared_job_differences(self.workflows, self.main_push_workflows)
        changed = {difference for difference in observed if difference[4] == "step-changed"}
        self._assert_matches_allowlist(
            observed - changed, SHARED_JOB_ALLOWLIST,
            "a shared job runs Bazel differently on pull requests")
        self._assert_matches_allowlist(
            {(name, job, step, scenario, digest)
             for name, job, scenario, step, _, digest in changed}, SHARED_STEP_ALLOWLIST,
            "a shared job runs a non-Bazel check differently on pull requests")

    def test_wasm_size_step_differs_only_in_its_reviewed_numeric_budget_mode(self):
        workflow = self.workflows["editor_wasm.yml"]
        job = _jobs(workflow)["build"]
        step = next(step for step in job["steps"]
                    if _step_name(step) == "Build and size-check Geode editor Wasm package")
        main = _other_step_view(workflow, job, step, "main-push")
        self.assertEqual(main[-1], [("PAYLOAD_BUDGET_MODE", "report")])
        for scenario in PULL_REQUEST_SCENARIOS:
            with self.subTest(scenario=scenario):
                pull_request = _other_step_view(workflow, job, step, scenario)
                self.assertEqual(pull_request[-1], [("PAYLOAD_BUDGET_MODE", "enforce")])
                self.assertEqual(main[:-1], pull_request[:-1])

    def test_wasm_size_parity_exception_rejects_mode_script_and_environment_drift(self):
        name = "editor_wasm.yml"
        step_name = "Build and size-check Geode editor Wasm package"
        approved = {key for key in SHARED_STEP_ALLOWLIST if key[:3] == (name, "build", step_name)}

        def observed(workflow):
            return {(file, job, step, scenario, digest)
                    for file, job, scenario, step, kind, digest in
                    shared_job_differences({name: workflow}, [name]) if kind == "step-changed"}

        self.assertEqual(observed(self.workflows[name]), approved)
        for mutation in ("all-report", "all-enforce", "extra-environment", "script"):
            with self.subTest(mutation=mutation):
                changed = copy.deepcopy(self.workflows[name])
                step = next(step for step in _jobs(changed)["build"]["steps"]
                            if _step_name(step) == step_name)
                if mutation in ("all-report", "all-enforce"):
                    step["env"]["PAYLOAD_BUDGET_MODE"] = mutation.removeprefix("all-")
                elif mutation == "extra-environment":
                    step["env"]["UNREVIEWED_FLAG"] = "1"
                else:
                    step["run"] += " --unexpected"
                self.assertNotEqual(observed(changed), approved)

    def test_event_dependent_expressions_are_compared(self):
        workflow = parse_workflow(
            "on:\n  push:\n  pull_request:\n"
            "jobs:\n  build:\n    env:\n"
            "      SCOPE: ${{ github.event_name == 'push' && '//...' || '//donner/base/...' }}\n"
            "    steps:\n      - name: Test\n        run: |\n          bazelisk test $SCOPE\n"
            "      - name: Narrow\n        run: |\n"
            "          bazelisk test ${{ github.event_name == 'push' && '--keep_going' || '' }} //a\n")
        differences = shared_job_differences({"x.yml": workflow}, ["x.yml"])
        for scenario in PULL_REQUEST_SCENARIOS:
            self.assertIn(("x.yml", "build", scenario, "Test", "target-main-only", "//..."),
                          differences)
            self.assertIn(("x.yml", "build", scenario, "Narrow", "flag-main-only",
                           "--keep_going"), differences)
        self.assertEqual(render("${{ runner.temp }}/x ${{ github.event_name }}", "main-push"),
                         "${{ runner.temp }}/x push")

    def test_commented_out_bazel_commands_run_nothing(self):
        self.assertEqual(bazel_invocations("# bazelisk test //...\n", {}), [])
        self.assertEqual(bazel_invocations("echo start  # bazelisk test //...\n", {}), [])
        # A `#` inside quotes starts no comment, including after an escaped quote.
        self.assertEqual(len(bazel_invocations('echo "lane #1"; bazelisk test //...\n', {})), 1)
        self.assertEqual(
            len(bazel_invocations('echo "label \\" #1"; bazelisk test //...\n', {})), 1)
        self.assertEqual(failure_masks('echo "label \\" #1"; set +e'),
                         {'echo "label \\" #1"; set +e'})
        self.assertEqual(bazel_invocations("echo it\\'s  # bazelisk test //...\n", {}), [])
        # Bash ANSI-C quoting: a backslash escapes the quote inside $'...'.
        self.assertEqual(
            len(bazel_invocations("echo $'label \\' #1'; bazelisk test //...\n", {})), 1)
        self.assertEqual(failure_masks("echo $'label \\' #1'; set +e"),
                         {"echo $'label \\' #1'; set +e"})
        # An escaped blank continues the word, so the `#` after it starts no comment. An escaped
        # backslash before the blank leaves the blank unescaped.
        masked = "echo label\\ #1; bazelisk test //... || true"
        self.assertEqual(len(bazel_invocations(masked + "\n", {})), 1)
        self.assertEqual(failure_masks(masked), {masked})
        self.assertEqual(len(bazel_invocations("echo a\\\t#1; bazelisk test //...\n", {})), 1)
        self.assertEqual(len(bazel_invocations("echo a\\\\\\ #1; bazelisk test //...\n", {})), 1)
        self.assertEqual(bazel_invocations("echo a\\\\ # bazelisk test //...\n", {}), [])
        self.assertEqual(bazel_invocations("echo a\\ #1  # bazelisk test //...\n", {}), [])
        # An even run of backslashes before a newline is no continuation, so the next line is a
        # comment of its own.
        self.assertEqual(bazel_invocations("echo a\\\\\n# bazelisk test //... || true\n", {}), [])
        self.assertEqual(failure_masks("echo a\\\\\n# bazelisk test //... || true"), set())
        # Inside an open quote the newline after an even run does not end the command.
        quoted_newline = 'echo "a\\\\\n# still quoted"; bazelisk test //... || true\n'
        self.assertEqual(len(bazel_invocations(quoted_newline, {})), 1)
        self.assertEqual(len(failure_masks(quoted_newline)), 1)
        # A YAML plain scalar has no escapes, so its ` #` starts a YAML comment.
        steps = parse_workflow("steps:\n  - run: echo a\\ #1; bazelisk test //... || true\n")
        self.assertEqual(steps["steps"][0]["run"], "echo a\\")
        quoted = bazel_invocations('bazelisk test --test_arg="a #b" //a\n', {})
        self.assertEqual(quoted[0]["targets"], {"//a"})
        self.assertEqual(failure_masks('echo "lane #1"; set +e\nbazelisk test //...'),
                         {'echo "lane #1"; set +e'})
        workflow = parse_workflow(
            "jobs:\n  main:\n    steps:\n      - name: Test\n        run: bazelisk test //...\n"
            "  pr:\n    steps:\n      - name: Test\n        run: \"# bazelisk test //...\"\n")
        self.assertIn(("Test", "step-main-only", ""),
                      invocation_differences(workflow, "main", "pr"))

    def test_trigger_shorthand_and_activity_types(self):
        self.assertEqual(triggers("push"), {"push": None})
        self.assertEqual(triggers(["push", "pull_request"]), {"push": None, "pull_request": None})
        self.assertTrue(_push_runs_on_main("push"))
        self.assertTrue(_push_runs_on_main(["push", "pull_request"]))
        workflows = {
            "short.yml": parse_workflow("on: push\n"),
            "both.yml": parse_workflow("on: [push, pull_request]\n"),
            "closed.yml": parse_workflow(
                "on:\n  push:\n  pull_request:\n    types: [closed]\n"),
            "edited.yml": parse_workflow(
                "on:\n  push:\n  pull_request:\n"
                "    types: [opened, synchronize, reopened, edited]\n"),
        }
        self.assertEqual(trigger_differences(workflows, sorted(workflows)),
                         {("short.yml", "push-only"), ("closed.yml", "pull-request-types")})

    def test_shared_steps_that_stop_running_bazel_are_reported(self):
        workflow = parse_workflow(
            "on:\n  push:\n  pull_request:\n"
            "jobs:\n  build:\n    steps:\n      - name: Test\n        run: |\n"
            "          ${{ github.event_name == 'pull_request' && '#' || '' }} bazelisk test //...\n")
        self.assertEqual(shared_job_differences({"x.yml": workflow}, ["x.yml"]),
                         {("x.yml", "build", scenario, "Test", "step-main-only", "")
                          for scenario in PULL_REQUEST_SCENARIOS})

    def test_policy_event_inputs_are_the_only_inputs_ignored(self):
        step = ("      - name: Cache\n        uses: ./.github/actions/buildbuddy-cache\n"
                "        with:\n          trusted-pr: ${{ needs.gatekeeper.outputs.trusted_pr }}\n"
                "          download-outputs: %s\n")
        workflow = parse_workflow("on:\n  push:\n  pull_request:\njobs:\n  build:\n"
                                  "    steps:\n" + step % "minimal")
        self.assertEqual(shared_job_differences({"x.yml": workflow}, ["x.yml"]), set())
        changed = parse_workflow(
            "on:\n  push:\n  pull_request:\njobs:\n  build:\n    steps:\n"
            + step % "${{ github.event_name == 'push' && 'minimal' || 'all' }}")
        self.assertEqual({difference[:5] for difference in
                          shared_job_differences({"x.yml": changed}, ["x.yml"])},
                         {("x.yml", "build", scenario, "Cache", "step-changed")
                          for scenario in PULL_REQUEST_SCENARIOS})
        # The exemption belongs to the action that reads the input, not to the input name.
        other_action = parse_workflow(
            "on:\n  push:\n  pull_request:\njobs:\n  build:\n    steps:\n"
            "      - name: Scan\n        uses: ./.github/actions/security-scan\n"
            "        with:\n          trusted-pr: ${{ github.event_name == 'push' && 'strict' "
            "|| 'off' }}\n")
        self.assertEqual({difference[:5] for difference in
                          shared_job_differences({"x.yml": other_action}, ["x.yml"])},
                         {("x.yml", "build", scenario, "Scan", "step-changed")
                          for scenario in PULL_REQUEST_SCENARIOS})

    def test_non_bazel_checks_render_event_expressions(self):
        lint = ("      - name: Lint\n        run: python3 tools/lint.py "
                "${{ github.event_name == 'push' && '--strict' || '' }}\n")
        workflow = parse_workflow("on:\n  push:\n  pull_request:\n"
                                  "jobs:\n  build:\n    steps:\n" + lint)
        observed = shared_job_differences({"x.yml": workflow}, ["x.yml"])
        self.assertEqual({difference[:5] for difference in observed},
                         {("x.yml", "build", scenario, "Lint", "step-changed")
                          for scenario in PULL_REQUEST_SCENARIOS})
        # The digest identifies the rendered definitions: an edit to the step changes it.
        stricter = parse_workflow("on:\n  push:\n  pull_request:\n"
                                  "jobs:\n  build:\n    steps:\n"
                                  + lint.replace("--strict", "--strict --all"))
        self.assertFalse(observed & shared_job_differences({"x.yml": stricter}, ["x.yml"]))
        replaced = parse_workflow("jobs:\n  main:\n    steps:\n" + lint
                                  + "  pr:\n    steps:\n" + lint)
        self.assertEqual(_kinds(invocation_differences(replaced, "main", "pr")),
                         {("Lint", "step-changed")})

    def test_branch_filters_follow_github_pattern_order(self):
        self.assertTrue(branch_filter_includes("main", ["**"]))
        self.assertFalse(branch_filter_includes("main", ["**", "!main"]))
        self.assertTrue(branch_filter_includes("main", ["!main", "**"]))
        self.assertTrue(branch_filter_includes("main", ["ma?n"]))
        self.assertFalse(branch_filter_includes("main", ["release/*"]))
        self.assertTrue(branch_filter_includes("main", ignore=["release/**"]))
        self.assertFalse(branch_filter_includes("main", ignore=["mai*"]))
        self.assertTrue(_push_runs_on_main({"push": {"branches-ignore": ["release/**"]}}))
        self.assertFalse(_push_runs_on_main({"push": {"tags": ["v*"]}}))
        workflows = {"x.yml": parse_workflow(
            "on:\n  push:\n    branches-ignore: [\"release/**\"]\n"
            "  pull_request:\n    branches: [\"**\", \"!main\"]\n")}
        self.assertEqual(trigger_differences(workflows, ["x.yml"]),
                         {("x.yml", "pull-request-branches")})

    def test_non_bazel_checks_compare_the_environment_they_read(self):
        main = ("    env:\n      MODE: strict\n      OTHER: a\n    steps:\n"
                "      - name: Lint\n        run: python3 tools/lint.py --mode \"$MODE\"\n")
        workflow = parse_workflow(
            "jobs:\n  main:\n" + main + "  pr:\n"
            + main.replace("MODE: strict", "MODE: fast").replace("OTHER: a", "OTHER: b"))
        self.assertEqual(_kinds(invocation_differences(workflow, "main", "pr")),
                         {("Lint", "step-changed")})
        unread = parse_workflow("jobs:\n  main:\n" + main + "  pr:\n"
                                + main.replace("OTHER: a", "OTHER: b"))
        self.assertEqual(invocation_differences(unread, "main", "pr"), set())

    def test_main_only_cache_saves_are_not_checks(self):
        workflow = parse_workflow(
            "on:\n  push:\n  pull_request:\n"
            "jobs:\n  build:\n    steps:\n"
            "      - name: Save cache\n"
            "        if: github.event_name != 'pull_request' && github.ref == 'refs/heads/main'\n"
            "        uses: actions/cache/save@v6\n"
            "      - name: Publish\n"
            "        if: github.event_name != 'pull_request'\n"
            "        run: echo publish\n")
        self.assertEqual(skipped_main_steps({"x.yml": workflow}, ["x.yml"]), {
            ("x.yml", "build", "Publish", "trusted-pr"),
            ("x.yml", "build", "Publish", "untrusted-pr"),
            ("x.yml", "build", "Publish", "renovate-pr"),
        })

    def test_hosted_subsets_run_main_steps_unchanged(self):
        observed = set()
        for (name, job_name, scenario), (subset, covered) in HOSTED_SUBSETS.items():
            workflow = self.workflows[name]
            observed |= {(name, job_name, subset) + difference
                         for difference in subset_differences(workflow, job_name, subset,
                                                              covered)}
            with self.subTest(subset=subset):
                self.assertEqual(subset_setup_differences(workflow, job_name, subset, covered),
                                 [])
                # Every variable the subset defines has main's value.
                main_environment = _environment(workflow, _jobs(workflow)[job_name])
                for key, value in _environment(workflow, _jobs(workflow)[subset]).items():
                    self.assertEqual(main_environment.get(key), value, key)
        self._assert_matches_allowlist(observed, SUBSET_ALLOWLIST,
                                       "a hosted subset runs a main step differently")

    def test_browser_partitions_keep_complete_nonoverlapping_selection(self):
        workflow = self.workflows["main.yml"]
        for job_name, scenario, positive in (
                ("macos", "main-push", "launches-browser"),
                ("macos-self-hosted", "trusted-pr", "launches-browser"),
                ("macos-browser", "trusted-pr", "requires_desktop_session"),
                ("macos-browser", "trusted-pr-fallback", "requires_desktop_session")):
            with self.subTest(job=job_name, scenario=scenario):
                self.assertTrue(job_gates(workflow, job_name, scenario))
                job = _jobs(workflow)[job_name]
                self.assertFalse(job.get("continue-on-error", False))
                profile, _ = _bazel_profile(workflow, job, scenario)
                invocations = profile["Test (browser targets)"]
                self.assertTrue(invocations)
                for invocation in invocations:
                    tags = invocation["tag_filters"]
                    self.assertEqual({positive}, {tag for tag in tags if not tag.startswith("-")})
                    self.assertEqual(job_name == "macos-self-hosted",
                                     "-requires_desktop_session" in tags)
        for other in ("main-push", "untrusted-pr", "renovate-pr", "trusted-pr-native-only"):
            self.assertFalse(job_gates(workflow, "macos-browser", other), other)

    def test_native_metal_job_runs_only_when_the_pull_request_reaches_its_suites(self):
        # The native Metal job reruns fixed suites uncached. It runs for a trusted pull request
        # whose affected targets reach those suites, or that falls back to //..., and never for
        # one that reaches neither; main and untrusted pull requests never route to it.
        workflow = self.workflows["main.yml"]
        for scenario, expected in (("trusted-pr", True), ("trusted-pr-fallback", True),
                                   ("trusted-pr-native-only", False), ("main-push", False),
                                   ("untrusted-pr", False), ("renovate-pr", False)):
            with self.subTest(scenario=scenario):
                self.assertEqual(expected, job_gates(workflow, "macos-metal", scenario))
        # A skipped run is an expected skip for the coverage contract, not a gap.
        manifest = self._coverage_manifest()
        metal = {name for name, entry in manifest["lanes"]["macos"]["Build and test"].items()
                 if any(c["job"] == "macos-metal" for c in entry.get("counterparts", []))}
        self.assertEqual({"shader-validation", "shader-memory-canary", "native-metal"}, metal)
        for name in metal:
            self.assertEqual("metal_affected", manifest["lanes"]["macos"]["Build and test"][name]["when"])

    def test_subset_order_problems_are_found(self):
        main = parse_workflow(
            "steps:\n      - name: Setup\n        run: x\n      - name: Profile\n"
            "        run: y\n      - name: Test\n        run: z\n")
        reordered = parse_workflow(
            "steps:\n      - name: Setup\n        run: x\n      - name: Test\n"
            "        run: z\n      - name: Profile\n        run: y\n"
            "      - name: Write bazelrc\n        run: echo test >> .bazelrc\n")
        self.assertEqual(subset_order_problems(main, reordered), [
            ("Profile", "out of order"),
            ("Write bazelrc", "not in main's job"),
        ])
        self.assertEqual(subset_order_problems(main, main), [])

    def test_hosted_subsets_run_only_for_affected_trusted_pull_requests(self):
        for (name, _, scenario), (subset, _) in HOSTED_SUBSETS.items():
            workflow = self.workflows[name]
            with self.subTest(subset=subset):
                # job_gates includes the jobs the subset needs.
                self.assertTrue(job_gates(workflow, subset, scenario))
                self.assertFalse(job_gates(workflow, subset, "trusted-pr-native-only"))
                # Main pushes, untrusted and Renovate PRs already run the whole hosted job.
                for other in ("main-push", "untrusted-pr", "renovate-pr"):
                    self.assertFalse(job_gates(workflow, subset, other), other)
                # An infrastructure change falls back to //..., which reaches every browser test.
                self.assertTrue(job_gates(workflow, subset, "trusted-pr-fallback"))

    def test_configs_are_compared_by_their_flags(self):
        # A lane's selection and constraints can live in .bazelrc configs, which the comparison
        # expands, so widening a config fails like widening the workflow.
        configs = _repository_configs()
        # Every config a workflow or action names is a repository config, so each one is
        # compared by its flags. A config only a runner's rc defines would be compared by name.
        actions = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml")).parent
        actions = sorted((actions.parent / "actions").glob("*/action.y*ml"))
        self.assertTrue(actions)
        for path in list(_workflow_paths()) + actions:
            text = "\n".join(line for line in path.read_text(encoding="utf-8").splitlines()
                             if not line.lstrip().startswith("#"))
            for name in set(re.findall(r"--config[= ]([a-z][A-Za-z0-9_.+-]*)", text)):
                with self.subTest(file=path.name, config=name):
                    self.assertIn(name, configs)
        # Main's hosted macOS job passes its quarantine as a config.
        self.assertEqual(
            bazelrc_configs.expand(
                "test", ["--config=hosted-macos-quarantine", "--config=other"], configs),
            ["--test_env=DONNER_HOSTED_MACOS_QUARANTINE=1", "--config=other"])
        # `test:` lines do not apply to a build.
        self.assertEqual(
            bazelrc_configs.expand("build", ["--config=hosted-macos-quarantine"], configs), [])
        widened = copy.deepcopy(configs)
        widened["hosted-macos-quarantine"] = [
            (kind, flags + ["--test_env=DONNER_WIDENED=1"])
            for kind, flags in widened["hosted-macos-quarantine"]]
        _CONFIGS[0] = widened
        try:
            observed = invocation_differences(self.workflows["main.yml"], "macos",
                                              "macos-self-hosted")
        finally:
            _CONFIGS[0] = configs
        difference = ("main.yml", "macos", "macos-self-hosted", "Build and test", "test-env-main-only",
                      "DONNER_WIDENED=1")
        self.assertIn(difference[3:], observed)
        self.assertNotIn(difference, INVOCATION_ALLOWLIST)

    @staticmethod
    def _coverage_manifest():
        return json.loads(Path(runfiles.Create().Rlocation(
            "donner/tools/ci/coverage_contract.json")).read_text(encoding="utf-8"))

    @staticmethod
    def _counterparts(manifest, lane=None):
        return [counterpart for name, phases in manifest["lanes"].items()
                if lane in (None, name)
                for classes in phases.values() for entry in classes.values()
                for counterpart in entry.get("counterparts", [])]

    def test_coverage_manifest_follows_the_workflow(self):
        # tools/ci/coverage_contract.json names main's phases and their pull request
        # counterparts by step name, so it must keep naming steps that exist and run.
        manifest = self._coverage_manifest()
        workflow = self.workflows["main.yml"]
        jobs = _jobs(workflow)
        self.assertEqual(set(COVERAGE_LANES), set(manifest["lanes"]))
        for lane, phases in manifest["lanes"].items():
            with self.subTest(lane=lane):
                # Every check main's hosted lane runs is a declared phase, and nothing else is.
                self.assertEqual(_coverage_phases(workflow, lane), set(phases))
        for counterpart in self._counterparts(manifest):
            with self.subTest(counterpart=counterpart):
                job_name, step_id = counterpart["job"], counterpart["step"]
                steps = {step.get("id"): step for step in jobs[job_name]["steps"]}
                self.assertIn(step_id, steps)
                self.assertTrue(job_gates(workflow, job_name, "trusted-pr-fallback"))
                # The job's receipt records its whole `steps` context.
                receipt = next(step for step in jobs[job_name]["steps"]
                               if _step_name(step) == "Record coverage receipt")
                self.assertEqual("python3 tools/ci_coverage_contract.py receipt %s" % job_name,
                                 str(receipt["run"]).strip())
                self.assertEqual({"STEPS_JSON": "${{ toJSON(steps) }}"}, receipt["env"])
                # A minimum test count is read from the step's own build event file.
                if counterpart.get("min_tests"):
                    self.assertTrue(any(
                        lanes.bep(step_id) in command for command in lane_commands(
                            steps[step_id])), step_id)

    def test_coverage_contract_job_needs_every_receipt_and_selection_source(self):
        # The contract job waits for every job whose receipt it reads.
        manifest = self._coverage_manifest()
        jobs = _jobs(self.workflows["main.yml"])
        declared = {(c["job"], c["step"]) for c in self._counterparts(manifest)}
        self.assertLessEqual({job for job, _ in declared},
                             set(_needs(jobs["coverage-contract"])))
        # The check reads its selections from these job outputs, so each must exist on a job
        # the contract job needs.
        for job, output in coverage_contract.SELECTION_SOURCES.values():
            with self.subTest(job=job, output=output):
                self.assertIn(output, jobs[job]["outputs"])
                self.assertIn(job, _needs(jobs["coverage-contract"]))

    def test_cmake_generator_check_runs_once_per_event(self):
        # gen_cmakelists.py --check runs once per event, in the CMake consumer step of the Linux
        # job that the event runs: main.yml's hosted or self-hosted Linux job for main pushes and
        # pull requests, and the CMake workflow's Linux consumer for a branch push before its pull
        # request exists. A documentation-only pull request runs none, since no path it can change
        # is a generator input (tools/ci/gatekeeper_tests.py). The label-gated CMake cache
        # qualification jobs repeat the main lane's command on purpose and never run without
        # their label, which no scenario carries.
        expected = {
            "main-push": ("main.yml", "linux"),
            "branch-push": ("cmake.yml", "linux"),
            "trusted-pr": ("main.yml", "linux-self-hosted"),
            "trusted-pr-native-only": ("main.yml", "linux-self-hosted"),
            "trusted-pr-fallback": ("main.yml", "linux-self-hosted"),
            "untrusted-pr": ("main.yml", "linux"),
            "renovate-pr": ("main.yml", "linux"),
        }
        branch_push = dict(SCENARIOS["main-push"], **{"github.ref": "refs/heads/topic"})
        with mock.patch.dict(SCENARIOS, {"branch-push": branch_push}):
            self.assertEqual(set(SCENARIOS), set(expected))
            for scenario, owner in expected.items():
                with self.subTest(scenario=scenario):
                    self.assertEqual({owner}, generator_check_jobs(self.workflows, scenario))
        # Only main pushes run the hosted macOS consumer.
        cmake = self.workflows["cmake.yml"]
        self.assertTrue(_event_starts(cmake, "main-push"))
        self.assertTrue(job_gates(cmake, "macos", "main-push"))
        for scenario in PULL_REQUEST_SCENARIOS:
            self.assertFalse(job_gates(cmake, "macos", scenario), scenario)
        with mock.patch.dict(SCENARIOS, {"branch-push": branch_push}):
            self.assertFalse(job_gates(cmake, "macos", "branch-push"))
        # The pattern sees both ways of running the check.
        for line in ("python3 tools/cmake/gen_cmakelists.py --check",
                     "tools/ci/cmake_consumer.sh build --check --ccache"):
            self.assertRegex(line, _GENERATOR_CHECK)
        self.assertNotRegex("tools/ci/cmake_consumer.sh build --ccache", _GENERATOR_CHECK)

    def test_every_allowlist_entry_has_a_reason(self):
        for name, (_, reason) in STEP_OUTPUTS.items():
            self.assertGreater(len(reason.strip()), 20, name)
        for allowlist in (TRIGGER_ALLOWLIST, MAIN_ONLY_JOBS, INVOCATION_ALLOWLIST,
                          STEP_ALLOWLIST, COUNTERPART_ALLOWLIST, FAILURE_SWALLOW_ALLOWLIST,
                          SHARED_JOB_ALLOWLIST, SHARED_STEP_ALLOWLIST, SUBSET_ALLOWLIST,
                          SUBSET_EXTRA_STEPS):
            for key, reason in allowlist.items():
                self.assertGreater(len(reason.strip()), 20, key)

    def test_invocation_comparison_reports_each_kind_of_difference(self):
        main = textwrap_job(
            'X_FLAGS: "--test_env=A=1"',
            "tools/ci_bazel_test.sh bazelisk test --config=ci --test_tag_filters=-manual,-perf "
            "$X_FLAGS //...",
        )
        replacement = textwrap_job(
            'X_FLAGS: "--test_env=B=1 --local_test_jobs=3"',
            "bazelisk --nohome_rc test --config=ci --test_tag_filters=-manual $X_FLAGS //...",
        )
        workflow = parse_workflow("jobs:\n  main:\n" + main + "  pr:\n" + replacement)
        self.assertEqual(invocation_differences(workflow, "main", "pr"), {
            ("Test", "exit-status-wrapper", ""),
            ("*", "startup-pr-only", "--nohome_rc"),
            ("*", "flag-pr-only", "--local_test_jobs=3"),
            ("Test", "test-env-main-only", "A=1"),
            ("Test", "test-env-pr-only", "B=1"),
            ("Test", "tag-filter-main-only", "-perf"),
        })


_replacing = lane_view.replacing


def _with_phase_args(lane, phase, edit):
    return lane_view.edited_lane(lane, edit, phases=(phase,))


def textwrap_job(environment, command):
    return ("    env:\n"
            "      %s\n"
            "    steps:\n"
            "      - name: Test\n"
            "        run: |\n"
            "          %s\n") % (environment, command)


def _update_lane_golden():
    """Writes LANE_GOLDEN for the checkout's main.yml and lane table."""
    root = Path(__file__).resolve().parent.parent
    workflow = parse_workflow((root / ".github/workflows/main.yml").read_text(encoding="utf-8"))
    (root / LANE_GOLDEN).write_text(json.dumps(lane_golden(workflow), indent=1, sort_keys=True)
                                    + "\n", encoding="utf-8")


if __name__ == "__main__":
    if sys.argv[1:] == ["--update-lane-golden"]:
        _update_lane_golden()
    else:
        unittest.main()
