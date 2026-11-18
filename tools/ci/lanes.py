"""The CI lane table: every Bazel command each main.yml job runs, as data.

Each lane is a job of .github/workflows/main.yml and each phase one of its steps, keyed by the
step id (or, for a step without one, a name given here). A step runs its phase with
`python3 tools/ci/lane.py phase <lane> <phase>`; `--dry-run` prints the commands below with the
run-time values filled in. tools/ci/lane.py owns how the actions run; this file only says what
runs, so a change here is a change to what CI builds and tests.

A command's arguments are words, with these run-time values:
  - SELECTED: the planner's target selection (`//...` on a fallback, otherwise the affected
    labels, which may be the //tools/ci:no_affected_targets placeholder), from the
    TARGETS_FALLBACK and TARGETS_AFFECTED environment the job passes.
  - HOSTED_BUILD_SELECTED: the same roots plus the full tree when verifying hosted cloud builds.
  - REMOTE_GPU_SELECTED: that selection with each locally isolated GPU suite swapped for its
    remote wrapper, from a MapRemoteGpu action earlier in the phase.
  - Split(name): an environment variable split into words, empty when unset. It carries flags
    that only exist at run time (the remote cache flags of the BuildBuddy action, and the
    `--xcode_version` the Xcode selection appends).
`$NAME` and `${NAME}` inside a word name a run-time variable of the runner (RUNNER_TEMP, DIAG_DIR
and the like); the phase fails when it is unset, as the step scripts did under `set -u`.

Standard library only: the phases run under each runner's system python3.
"""

import collections

Selected = collections.namedtuple("Selected", "kind")
SELECTED = Selected("affected")
REMOTE_GPU_SELECTED = Selected("remote-gpu")
HOSTED_BUILD_SELECTED = Selected("hosted-build")
Split = collections.namedtuple("Split", "name")

# Actions, run in order; the first failure ends the phase with its status unless `defer` is set,
# in which case the phase continues and ends with the first deferred failure.
Bazel = collections.namedtuple("Bazel", "args wrappers defer")
Command = collections.namedtuple("Command", "argv defer")
Notice = collections.namedtuple("Notice", "text")
# Fails when the selection is empty: that is a defect in the target derivation, not a change with
# nothing to test. With no patterns, bazel would fall back to its default target pattern (the
# working directory's package, which holds no tests here), build something nobody selected and
# report the empty test set as success. Unreachable while the planner selects
# //tools/ci:no_affected_targets for a proven empty change, or else //....
RequireSelection = collections.namedtuple("RequireSelection", "where")
MapRemoteGpu = collections.namedtuple("MapRemoteGpu", "")
# Ends the phase successfully when no selected test carries `tag`.
QueryTag = collections.namedtuple("QueryTag", "tag")
# Runs `fetch`; when it fails, removes the partially fetched repositories matching `repository`
# from the output base and fetches again.
RetryAfterReset = collections.namedtuple("RetryAfterReset", "fetch repository")
# Fails with `message` unless the coverage receipt records exactly `count` tests and executed tests
# for `step`.
RequireExecuted = collections.namedtuple("RequireExecuted", "step count message")

Phase = collections.namedtuple("Phase", "step actions")
Lane = collections.namedtuple("Lane", "startup phases")


def bazel(*args, wrappers=(), defer=False):
    return Bazel(tuple(args), tuple(wrappers), defer)


def command(*argv, defer=False):
    return Command(tuple(argv), defer)


# Maps only exit status 4 (no test targets in the selection) to success. A change-based selection
# can legitimately contain no test target: docs, tooling, a vendored subtree tested from its own
# workspace.
TEST_WRAPPER = ("tools/ci_bazel_test.sh",)


def watchdog(name):
    """The stall watchdog, teeing the console to the diagnostics directory's `name` folder."""
    return ("tools/ci/bazel_watchdog.sh", "$DIAG_DIR/%s/console.log" % name)


def diagnostics(name):
    """The profile, build events and execution log the self-hosted diagnostics report reads."""
    return ("--profile=$DIAG_DIR/%s/profile.gz" % name,
            "--build_event_json_file=$DIAG_DIR/%s/bep.json" % name,
            "--execution_log_compact_file=$DIAG_DIR/%s/exec.log.zst" % name)


def bep(name):
    """The build event file the coverage receipt reads for the step `name`."""
    return "--build_event_json_file=$RUNNER_TEMP/coverage-%s.bep.json" % name


def tags(*names):
    return "--test_tag_filters=" + ",".join(names)


NO_TAG_FILTER = tags()
FIXED_SUITE = ("--nocache_test_results", "--noskip_incompatible_explicit_targets")

# --- Linux. --------------------------------------------------------------------------------------

LINUX = ("--config=ci", "--config=latest_llvm")
BUILDBUDDY = Split("BUILDBUDDY_FLAGS")
# The self-hosted runner host may add a remote Bazel cache from its own rc files. Use it when
# healthy, but never fall back to local execution when remote execution is down or degraded: a
# CPU-capped runner executing the whole graph locally is the measured 2-3 h tail. 60 s, not 5 s:
# a healthy but busy endpoint takes longer on bulk transfers under a burst of jobs, and 60 s
# still fails a dead endpoint fast. These stay on the command line: a fallback flag anywhere in
# .bazelrc could outrank the BuildBuddy fragment's on every lane, so //tools:security_workflow_
# policy_tests (test_bazelrc_leaves_the_buildbuddy_fallback_to_the_fragment) bans them there.
SELF_HOSTED_REMOTE = ("--strategy=TestRunner=remote", "--remote_local_fallback=false",
                      "--incompatible_remote_local_fallback_for_remote_cache=false",
                      "--remote_timeout=60s")
# Build without the Bytes: the self-hosted runner is a thin remote-execution client, and tests run
# remotely, so only test logs and XML come back. These stay on the command line: an rc file would
# read the regular expression's backslashes as escapes.
DIAG_BUILD = ("--remote_download_outputs=minimal",)
DIAG_TEST = DIAG_BUILD + (r"--remote_download_regex=.*(test\.log|test\.xml|outputs\.zip)$",)

_SOAK = bazel("test", *LINUX, "--test_output=all", NO_TAG_FILTER, *FIXED_SUITE,
              bep("deterministic_soak_probe"), "//tools/ci:deterministic_fuzzer_soak")
_SELF_HOSTED_SOAK = bazel("test", *LINUX, "--test_output=all", *SELF_HOSTED_REMOTE,
                          NO_TAG_FILTER, *FIXED_SUITE, bep("deterministic_soak_probe"),
                          "//tools/ci:deterministic_fuzzer_soak")
WGPU_REFERENCE = ("--test_tag_filters=", "--test_env=XDG_RUNTIME_DIR=/tmp")

LINUX_LANES = {
    "linux": Lane(startup=(), phases={
        # Salt only the probe source so normal base-library compiles retain their action keys.
        # The regular build owns the full tree under the same cloud configuration.
        "buildbuddy_verify": Phase("Verify BuildBuddy remote execution", (
            bazel("build", *LINUX, BUILDBUDDY, "--remote_accept_cached=false", "--disk_cache=",
                  "--per_file_copt=^donner/base/AsyncifySuspendProbe[.]cc$@"
                  "-DBUILDBUDDY_RE_PROBE_${GITHUB_RUN_ID}_${GITHUB_RUN_ATTEMPT}",
                  "--execution_log_json_file=${RUNNER_TEMP}/buildbuddy-execution.json",
                  "//tools/ci:buildbuddy_compile_probe"),
            command("jq", "-s", "-e", 'any(.[]; .mnemonic == "CppCompile" and .runner == '
                    '"remote" and .cacheHit != true and (.commandArgs | '
                    'index("-DBUILDBUDDY_RE_PROBE_${GITHUB_RUN_ID}_${GITHUB_RUN_ATTEMPT}")) != null)', "${RUNNER_TEMP}/buildbuddy-execution.json"),
        )),
        "build": Phase("Build", (bazel("build", *LINUX, BUILDBUDDY, HOSTED_BUILD_SELECTED),)),
        "test": Phase("Test", (
            RequireSelection("the test step"),
            bazel("test", *LINUX, BUILDBUDDY, "--test_output=errors",
                  tags("-manual", "-perf", "-ci-remote-gpu", "-fuzz_soak"), SELECTED,
                  wrappers=TEST_WRAPPER),
        )),
        "deterministic_soak_probe": Phase("Test deterministic fuzzer soak wrapper", (_SOAK,)),
        "wgpu_resvg_reference": Phase("Test Linux WebGPU resvg reference", (
            bazel("test", *LINUX, "--test_output=errors", *WGPU_REFERENCE,
                  "//tools/ci:linux_wgpu_resvg_reference"),
        )),
        "linker_canary": Phase("Linker canary (non-lld linker support)", (
            bazel("build", "--config=ci", "--linkopt=-fuse-ld=gold", "//tools/ci:linker_canary"),
        )),
    }),
    "linux-self-hosted": Lane(startup=(), phases={
        # Keeps the server alive afterwards, so Build reuses this invocation's loading and
        # analysis instead of a fresh server start and about 30 s of analysis.
        "fetch": Phase("Fetch Bazel LLVM toolchain", (
            RetryAfterReset(bazel("fetch", *LINUX, *SELF_HOSTED_REMOTE, SELECTED),
                            "llvm_toolchain_linux_aarch64_llvm"),
        )),
        "build": Phase("Build", (
            bazel("build", *LINUX, *SELF_HOSTED_REMOTE, *DIAG_BUILD, *diagnostics("build"),
                  SELECTED, wrappers=watchdog("build")),
        )),
        # The paired targets share the same transitioned implementation and shard count. Their
        # no-local requirement fails closed if remote execution is unavailable.
        "test": Phase("Test", (
            RequireSelection("the test step"),
            MapRemoteGpu(),
            bazel("test", *LINUX, "--test_output=errors", *SELF_HOSTED_REMOTE, *DIAG_TEST,
                  tags("-manual", "-perf", "-local-gpu-isolated", "-fuzz_soak"),
                  *diagnostics("test"), REMOTE_GPU_SELECTED,
                  wrappers=TEST_WRAPPER + watchdog("test")),
        )),
        "wgpu_resvg_reference": Phase("Test Linux WebGPU resvg reference", (
            bazel("test", *LINUX, "--test_output=errors", *SELF_HOSTED_REMOTE, *DIAG_TEST,
                  *WGPU_REFERENCE, *diagnostics("test-wgpu"),
                  "//tools/ci:linux_wgpu_resvg_reference", wrappers=watchdog("test-wgpu")),
        )),
        "deterministic_soak_probe": Phase("Test deterministic fuzzer soak wrapper",
                                          (_SELF_HOSTED_SOAK,)),
        "local_xvfb": Phase("Test local Xvfb targets", (
            RequireSelection("the local Xvfb test step"),
            bazel("test", *LINUX, "--nocache_test_results", "--test_output=errors",
                  tags("requires_xvfb", "-fuzz_soak"), *SELF_HOSTED_REMOTE,
                  "--strategy=TestRunner=local", SELECTED, wrappers=TEST_WRAPPER),
        )),
    }),
}

# --- macOS. --------------------------------------------------------------------------------------

# Persistent self-hosted runners may have user or system Bazel rc files; their commands read only
# the settings declared in this repository.
NO_HOST_RC = ("--nohome_rc", "--nosystem_rc")
# The key-affecting flags of every macOS lane, identical in each (--config=lane-macos in .bazelrc,
# //tools:macos_cache_key_parity_tests). Xcode's libc++ marks std::format floating-point support
# as macOS 13.3+, so the deployment target is explicit; --config=ci-macos pins the action PATH.
# The Xcode selection appends --xcode_version through BAZEL_MACOS_BUILD_FLAGS.
MACOS_BUILD = ("--config=lane-macos", Split("BAZEL_MACOS_BUILD_FLAGS"))
# Each macOS lane's test environment, a .bazelrc config per lane that names the lane, so test
# results, which depend on the runner's GPU, are never shared between lanes.
MACOS_TEST = {lane: ("--config=lane-%s" % config,) for lane, config in (
    ("macos", "macos-hosted"), ("macos-browser", "macos-browser"),
    ("macos-self-hosted", "macos-vm"), ("macos-vm-qualification", "macos-vm-qualification"),
    ("macos-metal", "macos-metal"))}
# Enables the browser tests' timeout recorder (browser-stall-diagnostics.mjs). It records host
# load, memory and process call graphs, so only the ephemeral hosted job sets it; a persistent
# self-hosted runner never publishes them.
BROWSER_STALL = ("--test_env=DONNER_BROWSER_STALL_DIAGNOSTICS=1",)
# Exercises the Geode editor configuration through transitioned wrappers, which locate declared
# shared libraries consistently on clean workers.
GEODE_EDITOR = "//tools/ci:editor_geode"
BROWSER_ARCHIVE = "//tools/ci:mac_browser_archive"
# Regenerates the Xcode and Apple toolchain repositories for the selected Xcode; a restored
# external cache must not carry another Xcode's.
_REFRESH_XCODE = Phase("Refresh Xcode toolchain configuration", (bazel(
    "fetch", "--force", "--lockfile_mode=off",
    "--repo=@@bazel_tools+xcode_configure_extension+local_config_xcode",
    "--repo=@@apple_support++apple_cc_configure_extension+local_config_apple_cc",
    "--repo=@@apple_support++apple_cc_configure_extension+local_config_apple_cc_toolchains"),))


def _macos(lane):
    return MACOS_BUILD + MACOS_TEST[lane]


def _browser_preflight(lane, tag, *configs):
    """Qualifies the pinned Chromium archive when the selection reaches a test tagged `tag`."""
    return Phase("Qualify pinned Chromium archive", (
        RequireSelection("the browser preflight"),
        QueryTag(tag),
        bazel("test", "--config=ci", *configs, "--nocache_test_results", "--test_output=errors",
              *_macos(lane), BROWSER_ARCHIVE, wrappers=TEST_WRAPPER),
    ))


def _gpu_comparison(jobs, name):
    """An advisory rerun of the browser GPU tests, with a bounded timing summary."""
    events = "${RUNNER_TEMP}/browser-gpu-%s.bep.json" % name
    return Phase("Compare browser GPU tests (%s)" % name, (
        bazel("test", "--config=ci", "--test_output=errors", "--nocache_test_results",
              "--flaky_test_attempts=1", "--runs_per_test=1", "--local_test_jobs=%d" % jobs,
              "--keep_going", "--build_event_json_file=" + events, *_macos("macos"),
              *BROWSER_STALL, "//tools/ci:browser_gpu_comparison", defer=True),
        command("python3", "tools/browser_gpu_test_summary.py", events,
                "${RUNNER_TEMP}/browser-gpu-%s-summary.json" % name, defer=True),
    ))


def _metal(target, before=(), after=(), wrapped=True):
    return bazel("test", "--config=ci", *FIXED_SUITE, *before, "--test_output=errors", *after,
                 *_macos("macos-metal"), target, wrappers=TEST_WRAPPER if wrapped else ())


MACOS_LANES = {
    # Both gating test phases pass the hosted-macos-quarantine config (.bazelrc); only Test adds
    # the config that gives the Metal device tests API validation only. Each phase changes a build
    # option from the one before it, so each discards Bazel's analysis cache and re-analyzes,
    # about 15 to 35 s per phase on this image. Every macOS lane excludes the `fuzz_soak` mutation
    # targets, which run nightly or on demand in fuzz.yml under sanitizers; deterministic corpus
    # replay stays in the suites.
    "macos": Lane(startup=(), phases={
        "refresh_xcode": _REFRESH_XCODE,
        "test": Phase("Build and test", (
            RequireSelection("the test step"),
            # The hosted image ships Xcode without the offline Metal compiler, a separate
            # download. The MSL validation suite fails rather than skips without it, so this lane
            # drops it by tag and says so. Trusted PRs whose affected targets reach the Metal
            # suites run it on the capable self-hosted runner, and such a PR needs exact-head
            # full Metal validation before merge (docs/metal_validation.md).
            Notice("Excluding requires_metal_toolchain targets: the hosted macOS image has no "
                   "offline Metal compiler."),
            # Its virtual GPU cannot run shader validation either, so the tests that assert it
            # run only on the self-hosted real GPU.
            Notice("Excluding requires_shader_validation targets: the hosted macOS image's "
                   "virtual GPU cannot run Metal shader validation."),
            # Tests that launch a browser run in a later phase.
            bazel("test", "--config=ci", "--test_output=errors",
                  tags("-manual", "-perf", "-requires_metal_toolchain",
                       "-requires_shader_validation", "-ci-remote-gpu", "-launches-browser",
                       "-fuzz_soak"),
                  "--config=hosted-macos-quarantine", "--config=hosted-macos-unvalidated",
                  *_macos("macos"), *BROWSER_STALL, "--nobuild_tests_only", SELECTED,
                  wrappers=TEST_WRAPPER),
        )),
        # Browser tests hold wall-clock deadlines that a browser starved of CPU cannot meet. On
        # the three-core hosted runner, browsers co-scheduled with the rest of the suite missed
        # them, so every test that launches a browser runs after the suite, one at a time.
        "browser_preflight": _browser_preflight("macos", "launches-browser"),
        "browser_test": Phase("Test (browser targets)", (
            RequireSelection("the test step"),
            bazel("test", "--config=ci", "--test_output=errors", "--local_test_jobs=1",
                  bep("browser_test"),
                  tags("launches-browser", "-manual", "-perf", "-requires_metal_toolchain",
                       "-ci-remote-gpu", "-fuzz_soak"),
                  "--config=hosted-macos-quarantine", *_macos("macos"), *BROWSER_STALL,
                  SELECTED, wrappers=TEST_WRAPPER),
        )),
        "geode_test": Phase("Test (Geode editor lane)", (
            bazel("test", "--config=ci", "--config=geode", "--test_output=errors",
                  bep("geode_test"), *_macos("macos"), GEODE_EDITOR),
        )),
        "browser_gpu_serial": _gpu_comparison(1, "serial"),
        "browser_gpu_parallel": _gpu_comparison(4, "parallel"),
    }),
    # Headed-browser tests need a desktop session, which this native runner has.
    "macos-browser": Lane(startup=NO_HOST_RC, phases={
        "refresh_xcode": _REFRESH_XCODE,
        "browser_preflight": _browser_preflight("macos-browser", "requires_desktop_session"),
        "browser_test": Phase("Test (browser targets)", (
            RequireSelection("the test step"),
            bazel("test", "--config=ci", "--test_output=errors", "--local_test_jobs=1",
                  "--build_tests_only", bep("browser_test"),
                  tags("requires_desktop_session", "-manual", "-perf",
                       "-requires_metal_toolchain", "-ci-remote-gpu", "-fuzz_soak"),
                  *_macos("macos-browser"), SELECTED, wrappers=TEST_WRAPPER),
        )),
    }),
    # Every command passes --config=macos-vm, this runner's scheduling budget (.bazelrc), which
    # changes no action key.
    "macos-self-hosted": Lane(startup=NO_HOST_RC, phases={
        "refresh_xcode": _REFRESH_XCODE,
        "test": Phase("Build and test", (
            RequireSelection("the test step"),
            bazel("test", "--config=ci", "--config=macos-vm", "--test_output=errors",
                  tags("-manual", "-perf", "-requires_shader_validation",
                       "-requires_metal_toolchain", "-ci-remote-gpu", "-launches-browser",
                       "-fuzz_soak"),
                  *_macos("macos-self-hosted"), "--nobuild_tests_only", SELECTED, wrappers=TEST_WRAPPER),
        )),
        # Browser tests share the GPU with native replays and need their own serial phase;
        # tests that need a desktop session run in the macos-browser lane.
        "browser_preflight": _browser_preflight("macos-self-hosted", "launches-browser",
                                                "--config=macos-vm"),
        "browser_test": Phase("Test (browser targets)", (
            RequireSelection("the browser test step"),
            bazel("test", "--config=ci", "--config=macos-vm", "--test_output=errors",
                  "--local_test_jobs=1", bep("browser_test"),
                  tags("launches-browser", "-manual", "-perf", "-requires_desktop_session",
                       "-ci-remote-gpu", "-fuzz_soak"),
                  *_macos("macos-self-hosted"), SELECTED, wrappers=TEST_WRAPPER),
        )),
        "geode_test": Phase("Test (Geode editor lane)", (
            bazel("test", "--config=ci", "--config=macos-vm", "--config=geode",
                  "--test_output=errors", bep("geode_test"), *_macos("macos-self-hosted"),
                  GEODE_EDITOR),
        )),
        # An opt-in diagnostic with its existing deadline.
        "browser_context_diagnostic": Phase("Diagnose Chromium context lifecycle", (
            bazel("test", "--config=ci", "--config=macos-vm", "--nocache_test_results",
                  "--test_output=errors", "--local_test_jobs=1", NO_TAG_FILTER,
                  "--build_event_json_file=$RUNNER_TEMP/browser-context-diagnostic.bep.json",
                  *_macos("macos-self-hosted"), "--test_timeout=60",
                  "//tools/ci:chromium_context_diagnostic"),
        )),
    }),
    "macos-vm-qualification": Lane(startup=NO_HOST_RC, phases={
        "refresh_xcode": _REFRESH_XCODE,
        "qualification": Phase("Qualify concurrent VM workload", (
            bazel("test", "--config=ci", "--test_output=errors",
                  "--config=macos-vm-qualification", "--nocache_test_results", NO_TAG_FILTER,
                  "--noskip_incompatible_explicit_targets",
                  bep("qualification"), *_macos("macos-vm-qualification"),
                  "//tools/ci:macos_vm_qualification"),
        )),
        "require_executed": Phase("Require four executed qualification tests", (
            RequireExecuted("qualification", 4, "Expected four executed qualification tests"),
        )),
    }),
    # The only shader-validation gate in CI: the hosted image's virtual GPU cannot run it, so main
    # runs none. The capability probe and native suites are mandatory whenever this job runs.
    "macos-metal": Lane(startup=NO_HOST_RC, phases={
        "refresh_xcode": _REFRESH_XCODE,
        "metal_profile": Phase("Require full Metal shader validation", (
            _metal("//tools/ci:metal_full_validation", before=(NO_TAG_FILTER,), wrapped=False),
        )),
        "metal_preflight": Phase("Metal validation preflight", (
            _metal("//tools/ci:metal_preflight"),
        )),
        "metal_compiler": Phase("Validate offline Metal compiler", (
            _metal("//tools/ci:metal_offline_compiler"),
        )),
        "metal_native": Phase("Test remaining native Metal suite", (
            _metal("//tools/ci:metal_native", after=(bep("metal_native"),)),
        )),
    }),
}

# --- macOS from a Linux client. ------------------------------------------------------------------

# The advisory shadow lane: every action on a remote macOS executor, from a self-hosted Linux
# runner. Its one phase is its own script, which runs the preflight, both test phases, the
# starvation guard and the lane's budget, stages failed tests' outputs and prints only scrubbed,
# allow-listed lines (see "Remote macOS shadow lane" in docs/building.md). --budget-minutes keeps
# it inside the job's timeout-minutes with room to report and upload
# (//tools:ci_macos_remote_shadow_tests).
SHADOW_LANES = {
    "macos-remote-shadow": Lane(startup=(), phases={
        "shadow": Phase("Build and test for macOS on the remote executor", (
            RequireSelection("the macOS remote shadow lane"),
            command("python3", "tools/ci/macos_remote_shadow.py", "--budget-minutes", "70",
                    "--targets", SELECTED),
        )),
    }),
}

LANES = {**LINUX_LANES, **MACOS_LANES, **SHADOW_LANES}
