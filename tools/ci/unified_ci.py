"""Execute both target platforms on one trusted Linux Actions runner."""

import argparse
import contextlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

import lane
import lanes
import macos_remote_shadow as shadow
from remote_cmake import prepare

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import ci_coverage_contract as coverage

MAC = ["--config=remote-macos", "--config=unified-re-endpoint", "--config=ci-test-lane",
       "--incompatible_repo_env_ignores_action_env"]
LINUX = ["--config=ci", "--config=latest_llvm"]
REMOTE_TEST = ["--strategy=TestRunner=remote", "--noremote_local_fallback"]


def bazel(os_name):
    if os_name == "macos":
        # Keep the Linux analysis server, convenience symlinks and test logs independent.
        return ["bazelisk", "--max_idle_secs=60", "--host_jvm_args=-Xmx2048m", "--output_base=" + os.environ["MAC_BAZEL_BASE"]]
    return ["bazelisk"]


def mac_flags(command, serial=False):
    flags = ["--config=remote-macos-gpu", *MAC[1:],
             "--symlink_prefix=/", "--skip_incompatible_explicit_targets", "--jobs=1" if serial else "--jobs=4"]
    if command == "test":
        flags.extend([*REMOTE_TEST, "--test_output=errors", *lanes.DIAG_TEST,
                      "--nobuild_event_json_file_path_conversion"])
    return flags


class RemoteMac(lane._Context):
    """Keep existing phase selections, assertions and artifacts, changing the execution venue."""

    def bazel(self, action):
        command, *options = self.argv(action.args)
        if command not in ("build", "test"):
            raise ValueError("unexpected command in remote macOS lane: " + command)
        serial = "--local_test_jobs=1" in options or lanes.BROWSER_ARCHIVE in options
        options = [arg for arg in options if arg not in
                   ("--config=ci", "--config=macos-vm", "--local_test_jobs=1")]
        if command == "test" and not any(arg.startswith("--build_event_json_file=") for arg in options):
            options.append("--build_event_json_file=" + self.environ["UNIFIED_PHASE_BEP"])
        return [*self.argv(action.wrappers), *bazel("macos"), command, *mac_flags(command, serial), *options]

    def query(self, tag):
        return [*bazel("macos"), "query", super().query(tag)[-1]]


def routing(os_name, directory):
    config = MAC if os_name == "macos" else LINUX
    bep = directory / (os_name + "-routing.json")
    try:
        subprocess.run([*bazel(os_name), "build", *config, "--symlink_prefix=/", "--nobuild",
                        "--build_event_json_file=" + str(bep)], check=True)
        with bep.open() as events:
            return shadow.effective_executor(events)
    finally:
        bep.unlink(missing_ok=True)


def remote_consumer(os_name, inputs):
    config = MAC if os_name == "macos" else LINUX
    events = Path(os.environ["UNIFIED_PHASE_BEP"])
    subprocess.run([*bazel(os_name), "test", *config, *REMOTE_TEST, "--symlink_prefix=/",
                    "--build_event_json_file=" + str(events), "--nobuild_event_json_file_path_conversion",
                    *lanes.DIAG_TEST,
                    "--noskip_incompatible_explicit_targets", "--test_output=errors",
                    "--test_tag_filters=", "--nocache_test_results",
                    "--inject_repository=donner_cmake_inputs=" + str(inputs),
                    "@donner_cmake_inputs//:" + os_name], check=True)
    with events.open() as stream:
        results = [e["testResult"] for row in stream if "testResult" in (e := json.loads(row))]
    if len(results) != 1 or results[0].get("status") != "PASSED" or not shadow._ran_remotely(results[0]):
        raise RuntimeError("The native CMake consumer must execute and pass remotely")


def execute_lane(job, phases, directory, remote=False):
    temp = directory / job
    temp.mkdir()
    os.environ["RUNNER_TEMP"] = str(temp)
    outcomes = {}
    try:
        for phase in phases:
            print(f"::group::{job} {phase}", flush=True)
            outcomes[phase] = {"outcome": "failure"}
            event_path = temp / ("coverage-" + phase + ".bep.json")
            os.environ["UNIFIED_PHASE_BEP"] = str(event_path)
            try:
                if phase == "cmake_consumer":
                    remote_consumer("macos" if remote else "linux", directory / "cmake-inputs")
                else:
                    context = RemoteMac if remote else lane._Context
                    rc = lane.run_phase(job, phase, context_type=context)
                    if rc:
                        raise subprocess.CalledProcessError(rc, [job, phase])
                outcomes[phase]["outcome"] = "success"
            finally:
                if remote and outcomes[phase]["outcome"] == "failure" and event_path.exists():
                    with event_path.open() as stream:
                        shadow.stage_failed_test_outputs(stream, directory, job + "-" + phase, os.environ["MAC_BAZEL_BASE"],
                                                         shadow.MAX_ARTIFACT_TOTAL_BYTES)
                print("::endgroup::", flush=True)
    finally:
        receipt = coverage.make_receipt(job, outcomes, temp)
        (temp / "coverage-receipt.json").write_text(json.dumps(receipt))


def run_os(os_name, directory):
    fallback = os.environ["TARGETS_FALLBACK"] == "true"
    if os_name == "linux":
        phases = ["fetch", "build", "test"]
        if os.environ["WGPU_REFERENCE"] == "true":
            phases.append("wgpu_resvg_reference")
        execute_lane("linux-self-hosted", phases + ["deterministic_soak_probe", "local_xvfb", "cmake_consumer"], directory)
        return
    probe = directory / "darwin-probe.json"
    subprocess.run([*bazel("macos"), "test", *MAC, *REMOTE_TEST, "--symlink_prefix=/",
                    "--nocache_test_results", "--test_tag_filters=", "--test_output=errors",
                    "--build_event_json_file=" + str(probe), shadow.ROUTING_PROBE], check=True)
    with probe.open() as events:
        if not shadow.probe_executed_remotely(events):
            raise RuntimeError("Darwin arm64 probe did not execute remotely")
    phases = ["build", "test", "browser_preflight", "browser_test", "geode_test"]
    if os.environ["CMAKE_AFFECTED"] == "true":
        phases.append("cmake_consumer")
    work = [("macos-self-hosted", phases)]
    if fallback or os.environ["BROWSER_AFFECTED"] == "true":
        work.append(("macos-browser", ["browser_preflight", "browser_test"]))
    if fallback or os.environ["METAL_AFFECTED"] == "true":
        work.append(("macos-metal", ["metal_profile", "metal_preflight", "metal_compiler", "metal_native"]))
    failed = []
    for job, phases in work:
        try:
            execute_lane(job, phases, directory, remote=True)
        except subprocess.CalledProcessError:
            failed.append(job)
    if failed:
        raise RuntimeError("Failed macOS lanes: " + ", ".join(failed))


def stop_children(children, grace=10):
    # A phase can leave a descendant after its leader exits. Account for the
    # group itself until the deadline, reaping leaders while we wait.
    groups = {child.pid for child in children}
    for group in groups:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(group, signal.SIGTERM)
    deadline = time.monotonic() + grace
    while groups and time.monotonic() < deadline:
        for child in children:
            child.poll()
        for group in groups.copy():
            try:
                os.killpg(group, 0)
            except ProcessLookupError:
                groups.remove(group)
        if groups:
            time.sleep(0.1)
    for group in groups:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(group, signal.SIGKILL)
    for child in children:
        child.wait(timeout=10)


def interrupted(signum, frame):
    raise KeyboardInterrupt


def supervise(commands):
    children = []
    try:
        for command in commands:
            children.append(subprocess.Popen(command, start_new_session=True))
        return [child.wait() for child in children]
    finally:
        stop_children(children)


def prepare_job(directory):
    # Generate before starting either lane's analysis JVM on a memory-limited runner.
    prepare(ROOT, directory / "cmake-inputs")
    endpoint = routing("linux", directory)
    if not endpoint or routing("macos", directory) != endpoint:
        raise RuntimeError("Linux and macOS require the same configured remote executor")


def combined(directory):
    signal.signal(signal.SIGTERM, interrupted)
    directory.mkdir()
    if not os.environ.get("TARGETS_AFFECTED") and os.environ["TARGETS_FALLBACK"] != "true":
        raise ValueError("empty target selection")
    diagnostics = directory / "linux-diagnostics"
    for name in ("fetch", "build", "test", "test-wgpu"):
        (diagnostics / name).mkdir(parents=True)
    os.environ["DIAG_DIR"] = str(diagnostics)
    targets = ["//..."] if os.environ["TARGETS_FALLBACK"] == "true" else os.environ["TARGETS_AFFECTED"].split()
    (diagnostics / "target-list.txt").write_text("\n".join(targets) + "\n")
    (diagnostics / "manifest.txt").write_text(
        f"run_id={os.environ.get('GITHUB_RUN_ID')} attempt={os.environ.get('GITHUB_RUN_ATTEMPT')} "
        f"sha={os.environ.get('GITHUB_SHA')}\ntarget_count={len(targets)}\n")
    os.environ["DONNER_CI_XCODE_IDENTITY"] = shadow.xcode_identity()
    os.environ["BAZEL_MACOS_BUILD_FLAGS"] = ""
    os.environ["MAC_BAZEL_BASE"] = str(Path(os.environ["RUNNER_TOOL_CACHE"]) / "donner-unified-macos")
    command = [sys.executable, __file__, str(directory), "--os"]
    if any(supervise([[*command, "prepare"]])):
        return 1
    results = supervise([[*command, os_name] for os_name in ("linux", "macos")])
    receipts = coverage._load_receipts(directory)
    selection = coverage.selection_from_needs(json.loads(os.environ["NEEDS_JSON"]))
    problems, exceptions = coverage.check(json.loads((ROOT / coverage.MANIFEST).read_text()), receipts, selection)
    coverage._report(problems, exceptions, receipts)
    return int(any(results) or bool(problems))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--os", choices=["prepare", "linux", "macos"])
    args = parser.parse_args()
    if args.os == "prepare":
        prepare_job(args.directory)
    elif args.os:
        run_os(args.os, args.directory)
    else:
        sys.exit(combined(args.directory))
