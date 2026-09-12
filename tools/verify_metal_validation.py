#!/usr/bin/env python3
"""Run full Metal validation on a clean commit and retain a local verification receipt."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from urllib.parse import unquote, urlparse
import xml.etree.ElementTree as ET

from metal_validation_profile import parse_profile


PACKAGE = "//donner/gpu/metal/tests:"
CAPABILITY = PACKAGE + "metal_full_validation_required"
TARGETS = tuple(PACKAGE + name for name in (
    "metal_buffer_bounds_tests", "metal_color_matrix_tests", "metal_queue_writes_tests",
    "metal_solid_fill_tests", "metal_sub_rectangle_copy_tests",
    "metal_shader_memory_validation_tests",
))
MAX_LOG_BYTES = 16 * 1024 * 1024


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def git(root, *args):
    result = subprocess.run(["git", "-C", str(root), *args], check=True,
                            capture_output=True, text=True)
    return result.stdout.strip()


def clean_revision(root):
    if git(root, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("Full validation requires a clean, committed checkout")
    return git(root, "rev-parse", "HEAD")


def validate_xml(data):
    root = ET.fromstring(data)
    cases = list(root.iter("testcase"))
    if not cases:
        raise ValueError("Test XML contains no executed cases")
    for element in root.iter():
        for attr in ("failures", "errors", "skipped", "disabled"):
            if int(element.get(attr, "0")):
                raise ValueError("Full validation cannot contain failures or skipped cases")
        if element.tag in ("failure", "error", "skipped"):
            raise ValueError("Full validation contains a failed or skipped case")
    for case in cases:
        if case.get("status") == "notrun" or case.get("result") in ("suppressed", "skipped"):
            raise ValueError("Full validation contains an unexecuted case")
    return len(cases)


def output_digest(uri, local_path):
    """Bind downloaded local output to the exact output named by Bazel's event stream."""
    parsed = urlparse(uri)
    if parsed.scheme == "bytestream":
        match = re.search(r"/blobs/([0-9a-f]{64})/(\d+)$", parsed.path)
        if not match:
            raise ValueError("Unrecognized content-addressed Bazel output")
        expected_hash, expected_size = match.group(1), int(match.group(2))
    elif parsed.scheme == "file" and parsed.netloc in ("", "localhost"):
        if Path(unquote(parsed.path)).resolve() != local_path.resolve():
            raise ValueError("Bazel output points at a different local file")
        expected_hash, expected_size = None, None
    else:
        raise ValueError("Unsupported Bazel output URI")
    size = local_path.stat().st_size
    if size > MAX_LOG_BYTES:
        raise ValueError("Bazel output exceeds the receipt size bound")
    data = local_path.read_bytes()
    digest = sha256(data)
    if expected_hash is not None and (digest != expected_hash or len(data) != expected_size):
        raise ValueError("Downloaded test output does not match this invocation")
    return data, {"sha256": digest, "size": len(data)}


def collect_results(bep_path, testlogs, expected, destination):
    summaries, results, started, finished = {}, [], None, None
    configurations = set()
    with bep_path.open(encoding="utf-8") as stream:
        for line in stream:
            event = json.loads(line)
            if "started" in event:
                if started is not None:
                    raise ValueError("Multiple invocations were combined in one event stream")
                started = event["started"]
            if "finished" in event:
                finished = event["finished"]
            if "testSummary" in event:
                identity = event["id"]["testSummary"]
                label = identity["label"]
                if label in summaries:
                    raise ValueError("Duplicate target summaries cannot qualify")
                summaries[label] = {**event["testSummary"],
                                    "configuration_id": identity["configuration"]["id"]}
            if "testResult" in event:
                identity = event["id"]["testResult"]
                results.append((identity, event["testResult"]))
                configurations.add(identity["configuration"]["id"])
    if (finished is None or finished.get("exitCode", {}).get("name") != "SUCCESS" or
            finished.get("exitCode", {}).get("code", 0) != 0):
        raise ValueError("The event stream does not record a successful completed build")
    if started is None or set(summaries) != set(expected):
        raise ValueError("Invocation did not complete every required test target")
    for summary in summaries.values():
        if (summary.get("overallStatus") != "PASSED" or
                summary.get("attemptCount", 1) != 1 or summary.get("runCount", 1) != 1):
            raise ValueError("Full validation requires one successful attempt per target")
    expected_identities = set()
    for label, summary in summaries.items():
        shard_count = max(1, summary.get("shardCount", 0))
        if summary.get("shardCount", 0) < 0 or summary.get("totalRunCount") != shard_count:
            raise ValueError("Test summary has inconsistent shard/run counts")
        expected_identities.update((label, shard) for shard in range(1, shard_count + 1))
    seen = set()
    records = []
    for identity, result in results:
        label = identity["label"]
        execution = result.get("executionInfo", {})
        if (label not in expected or result.get("status") != "PASSED" or result.get("cachedLocally") or
                result.get("cachedRemotely") or execution.get("cachedRemotely")):
            raise ValueError("Unexpected, failed, or cached test result")
        key = (label, identity.get("shard", 1))
        if key not in expected_identities or key in seen:
            raise ValueError("Unexpected or duplicate test shard")
        if identity["configuration"]["id"] != summaries[label]["configuration_id"]:
            raise ValueError("Test result and summary configurations differ")
        seen.add(key)
        if identity.get("attempt", 1) != 1 or identity.get("run", 1) != 1:
            raise ValueError("Repeated test attempts cannot qualify a receipt")
        package, name = label.removeprefix("//").split(":", 1)
        directory = testlogs / package / name
        shards = max(1, summaries[label].get("shardCount", 0))
        shard = identity.get("shard", 1)
        if shards > 1:
            directory /= f"shard_{shard}_of_{shards}"
        outputs = {item["name"]: item["uri"] for item in result["testActionOutput"]}
        record = {"target": label, "shard": shard,
                  "strategy": result.get("executionInfo", {}).get("strategy", "unknown")}
        for filename in ("test.log", "test.xml"):
            data, digest = output_digest(outputs[filename], directory / filename)
            saved = f"{name}-{shard}-{filename}"
            (destination / saved).write_bytes(data)
            record[filename] = {**digest, "file": saved}
            if filename == "test.xml":
                record["cases"] = validate_xml(data)
            elif label == CAPABILITY:
                if parse_profile(data.decode("utf-8")) != "full":
                    raise ValueError("The execution device did not prove full shader validation")
        records.append(record)
    if seen != expected_identities:
        raise ValueError("A required shard has no fresh execution evidence")
    return {"invocation_id": started["uuid"], "bazel_version": started["buildToolVersion"],
            "configuration_ids": sorted(configurations), "tests": records}


def run_logged(command, root, output):
    with output.open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise ValueError(f"Verification command failed; inspect {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bazel", default="bazelisk")
    parser.add_argument("--config", action="append", default=[])
    parser.add_argument("--bazelrc", action="append", type=Path, default=[],
                        help="Explicitly approved configuration file; ambient home rc is excluded")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--receipt", type=Path, required=True,
                        help="Local JSON receipt; accompanying logs remain local")
    args = parser.parse_args()
    if args.jobs < 1 or any(not re.fullmatch(r"[A-Za-z0-9_.-]+", value) for value in args.config):
        parser.error("Invalid job count or configuration name")
    root = Path(git(Path.cwd(), "rev-parse", "--show-toplevel"))
    revision = clean_revision(root)
    receipt = args.receipt.resolve()
    receipt.parent.mkdir(parents=True, exist_ok=True)
    if receipt.exists():
        raise ValueError("Refusing to replace an existing verification receipt")
    artifacts = Path(tempfile.mkdtemp(prefix=receipt.stem + "-", dir=receipt.parent))
    rc_files = [path.resolve() for path in args.bazelrc]
    rc_hashes = {str(path): sha256(path.read_bytes()) for path in rc_files}
    startup = [args.bazel, "--nohome_rc", *("--bazelrc=" + str(path) for path in rc_files)]
    configs = ["--config=ci", *("--config=" + value for value in args.config)]
    common = [*configs, f"--jobs={args.jobs}", "--remote_local_fallback=false",
              "--test_tag_filters=", "--test_filter=", "--runs_per_test=1",
              "--flaky_test_attempts=1", "--nocache_test_results", "--test_output=all"]
    for name in ("MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION", "MTL_SHADER_VALIDATION_TEXTURE_USAGE",
                 "MTL_SHADER_VALIDATION_GLOBAL_MEMORY", "MTL_SHADER_VALIDATION_THREADGROUP_MEMORY",
                 "MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING", "MTL_SHADER_VALIDATION_ABORT_ON_FAULT",
                 "MTL_SHADER_VALIDATION_REPORT_TO_STDERR", "DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER"):
        common.append(f"--test_env={name}=1")
    common.extend([
        "--test_env=MTL_SHADER_VALIDATION_DEFAULT_STATE=all",
        "--test_env=MTL_SHADER_VALIDATION_DISABLE_PIPELINES=",
        "--test_env=MTL_SHADER_VALIDATION_ENABLE_PIPELINES=",
    ])
    invocations = []
    for phase, targets, extra in (
        ("capability", (CAPABILITY,), []),
        ("metal", TARGETS, ["--test_arg=--gtest_filter=*", "--test_arg=--gtest_repeat=1"]),
    ):
        bep = artifacts / f"{phase}.bep.json"
        command = [*startup, "test", *common, *extra,
                   f"--build_event_json_file={bep}", *targets]
        run_logged(command, root, artifacts / f"{phase}.log")
        testlogs = (root / "bazel-testlogs").resolve()
        evidence = collect_results(bep, testlogs, targets, artifacts)
        evidence["command"] = command
        evidence["bep_sha256"] = sha256(bep.read_bytes())
        invocations.append(evidence)
    toolchain_path = artifacts / "toolchain.json"
    query = f'mnemonic("ObjcCompile|CppCompile", deps({CAPABILITY}))'
    with toolchain_path.open("w", encoding="utf-8") as output, \
            (artifacts / "toolchain.log").open("w", encoding="utf-8") as error:
        subprocess.run([*startup, "aquery", *configs, "--output=jsonproto", query],
                       cwd=root, stdout=output, stderr=error, check=True)
    actions = json.loads(toolchain_path.read_text())["actions"]
    if not actions:
        raise ValueError("No compiler action was recorded for the native capability probe")
    toolchains = []
    for action in actions:
        environment = {item["key"]: item["value"] for item in action.get("environmentVariables", [])}
        toolchains.append({"mnemonic": action["mnemonic"], "action_key": action["actionKey"],
                           "arguments_sha256": sha256(json.dumps(action.get("arguments", [])).encode()),
                           "sdk_version": environment.get("APPLE_SDK_VERSION_OVERRIDE"),
                           "xcode_version": environment.get("XCODE_VERSION_OVERRIDE")})
    if clean_revision(root) != revision:
        raise ValueError("Source revision changed during verification")
    if any(sha256(path.read_bytes()) != rc_hashes[str(path)] for path in rc_files):
        raise ValueError("Explicit configuration changed during verification")
    dependencies = {}
    for name in (".bazelversion", "MODULE.bazel", "MODULE.bazel.lock", "tools/python/MODULE.bazel",
                 "tools/python/requirements.txt"):
        path = root / name
        if path.is_file():
            dependencies[name] = sha256(path.read_bytes())
    document = {"schema": 1, "status": "passed", "source_revision": revision,
                "source_tree": git(root, "rev-parse", revision + "^{tree}"),
                "verified_at": datetime.now(timezone.utc).isoformat(), "profile": "full",
                "dependency_hashes": dependencies, "explicit_bazelrc_hashes": rc_hashes,
                "invocations": invocations,
                "toolchain_actions": toolchains, "toolchain_sha256": sha256(toolchain_path.read_bytes()),
                "local_artifacts": artifacts.name}
    temporary = artifacts / "receipt.json"
    temporary.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    os.chmod(temporary, 0o600)
    os.replace(temporary, receipt)
    print(f"Full Metal validation passed for {revision}; local receipt: {receipt}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError, ET.ParseError, KeyError) as error:
        print(f"Metal validation did not qualify: {error}", file=sys.stderr)
        sys.exit(1)
