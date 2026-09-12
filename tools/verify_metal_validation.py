#!/usr/bin/env python3
"""Run full Metal validation on a clean commit and retain a local verification receipt."""

import argparse
from dataclasses import dataclass, field
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
from xml.parsers import expat

from metal_validation_profile import parse_profile


PACKAGE = "//donner/gpu/metal/tests:"
CAPABILITY = PACKAGE + "metal_full_validation_required"
TARGETS = tuple(PACKAGE + name for name in (
    "metal_buffer_bounds_tests", "metal_color_matrix_tests", "metal_queue_writes_tests",
    "metal_solid_fill_tests", "metal_sub_rectangle_copy_tests",
    "metal_shader_memory_validation_tests",
))
MAX_LOG_BYTES = 16 * 1024 * 1024
MAX_XML_BYTES = 16 * 1024 * 1024
MAX_XML_DEPTH = 64
MAX_XML_ELEMENTS = 100_000
MAX_XML_ATTRIBUTES = 64
MAX_XML_NAMESPACE_PREFIX_CHARS = 128
MAX_XML_NAMESPACE_URI_CHARS = 1024
MAX_XML_QNAME_CHARS = 2048
MAX_XML_EXPANDED_NAME_CHARS = 1024 * 1024
XML_FAILURE_COUNTS = frozenset(("failures", "errors", "skipped", "disabled"))
XML_FAILURE_ELEMENTS = frozenset(("failure", "error", "skipped", "disabled"))


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


def _xml_local_name(name):
    return name.rsplit("}", 1)[-1]


def _validate_case_attribute(name, value):
    if name == "status" and value != "run":
        raise ValueError("Full validation contains an unexecuted case")
    if name == "result" and value != "completed":
        raise ValueError("Full validation contains an unexecuted case")


def _validate_xml_attributes(element, attributes):
    for expanded_name, value in attributes.items():
        name = _xml_local_name(expanded_name)
        if name in XML_FAILURE_COUNTS and int(value):
            raise ValueError("Full validation cannot contain failures or skipped cases")
        if element == "testcase":
            _validate_case_attribute(name, value)


class _TestXmlReader:
    """Count successful cases without building a tree or accepting declarations."""

    def __init__(self):
        self.cases = 0
        self.depth = 0
        self.elements = 0
        self.pending_namespaces = 0
        self.expanded_name_characters = 0

    def namespace_started(self, prefix, uri):
        if len(prefix or "") > MAX_XML_NAMESPACE_PREFIX_CHARS:
            raise ValueError("Test XML exceeds the namespace prefix limit")
        if len(uri or "") > MAX_XML_NAMESPACE_URI_CHARS:
            raise ValueError("Test XML exceeds the namespace URI limit")
        self.pending_namespaces += 1
        if self.pending_namespaces > MAX_XML_ATTRIBUTES:
            raise ValueError("Test XML exceeds the attribute limit")

    def element_started(self, expanded_name, attributes):
        self.depth += 1
        self.elements += 1
        self._check_bounds(len(attributes) + self.pending_namespaces)
        self.pending_namespaces = 0
        self._charge_expanded_name(expanded_name)
        for attribute_name in attributes:
            self._charge_expanded_name(attribute_name)
        name = _xml_local_name(expanded_name)
        if name in XML_FAILURE_ELEMENTS:
            raise ValueError("Full validation contains a failed or skipped case")
        _validate_xml_attributes(name, attributes)
        if name == "testcase":
            self.cases += 1

    def _charge_expanded_name(self, name):
        if len(name) > MAX_XML_QNAME_CHARS:
            raise ValueError("Test XML exceeds the expanded QName limit")
        self.expanded_name_characters += len(name)
        if self.expanded_name_characters > MAX_XML_EXPANDED_NAME_CHARS:
            raise ValueError("Test XML exceeds the expanded name budget")

    def _check_bounds(self, attribute_count):
        if self.depth > MAX_XML_DEPTH:
            raise ValueError("Test XML exceeds the depth limit")
        if self.elements > MAX_XML_ELEMENTS:
            raise ValueError("Test XML exceeds the element limit")
        if attribute_count > MAX_XML_ATTRIBUTES:
            raise ValueError("Test XML exceeds the attribute limit")

    def element_ended(self, _name):
        # Matching end tags reuse names already charged when their elements started.
        self.depth -= 1


def _reject_xml_declaration(*_arguments):
    raise ValueError("Test XML must not contain DTD or entity declarations")


def _test_xml_parser(reader):
    parser = expat.ParserCreate(namespace_separator="}")
    parser.SetParamEntityParsing(expat.XML_PARAM_ENTITY_PARSING_NEVER)
    parser.StartDoctypeDeclHandler = _reject_xml_declaration
    parser.EntityDeclHandler = _reject_xml_declaration
    parser.UnparsedEntityDeclHandler = _reject_xml_declaration
    parser.ExternalEntityRefHandler = _reject_xml_declaration
    parser.StartNamespaceDeclHandler = reader.namespace_started
    parser.StartElementHandler = reader.element_started
    parser.EndElementHandler = reader.element_ended
    if hasattr(parser, "SetReparseDeferralEnabled"):
        parser.SetReparseDeferralEnabled(True)
    return parser


def validate_xml(data):
    if not isinstance(data, bytes):
        raise ValueError("Test XML must be supplied as bytes")
    if len(data) > MAX_XML_BYTES:
        raise ValueError("Test XML exceeds the byte limit")
    if expat.version_info < (2, 6, 0):
        raise ValueError("Test XML requires Expat 2.6 or newer")
    reader = _TestXmlReader()
    parser = _test_xml_parser(reader)
    try:
        parser.Parse(data, True)
    except expat.ExpatError as error:
        raise ValueError("Malformed test XML") from error
    if not reader.cases:
        raise ValueError("Test XML contains no executed cases")
    return reader.cases


def _expected_output_digest(uri, local_path):
    parsed = urlparse(uri)
    if parsed.scheme == "bytestream":
        match = re.search(r"/blobs/([0-9a-f]{64})/(\d+)$", parsed.path)
        if not match:
            raise ValueError("Unrecognized content-addressed Bazel output")
        return match.group(1), int(match.group(2))
    if parsed.scheme != "file" or parsed.netloc not in ("", "localhost"):
        raise ValueError("Unsupported Bazel output URI")
    if Path(unquote(parsed.path)).resolve() != local_path.resolve():
        raise ValueError("Bazel output points at a different local file")
    return None, None


def _read_bounded_output(local_path):
    if local_path.stat().st_size > MAX_LOG_BYTES:
        raise ValueError("Bazel output exceeds the receipt size bound")
    with local_path.open("rb") as stream:
        data = stream.read(MAX_LOG_BYTES + 1)
    if len(data) > MAX_LOG_BYTES:
        raise ValueError("Bazel output exceeds the receipt size bound")
    return data


def output_digest(uri, local_path):
    """Bind downloaded local output to the exact output named by Bazel's event stream."""
    expected_hash, expected_size = _expected_output_digest(uri, local_path)
    data = _read_bounded_output(local_path)
    digest = sha256(data)
    if expected_hash is not None and (digest != expected_hash or len(data) != expected_size):
        raise ValueError("Downloaded test output does not match this invocation")
    return data, {"sha256": digest, "size": len(data)}


@dataclass
class _InvocationEvents:
    summaries: dict = field(default_factory=dict)
    results: list = field(default_factory=list)
    started: dict | None = None
    finished: dict | None = None
    configurations: set = field(default_factory=set)

    def record(self, event):
        if "started" in event:
            self._record_started(event["started"])
        if "finished" in event:
            self._record_finished(event["finished"])
        if "testSummary" in event:
            self._record_summary(event)
        if "testResult" in event:
            identity = event["id"]["testResult"]
            self.results.append((identity, event["testResult"]))
            self.configurations.add(identity["configuration"]["id"])

    def _record_started(self, started):
        if self.started is not None:
            raise ValueError("Multiple invocations were combined in one event stream")
        self.started = started

    def _record_finished(self, finished):
        if self.finished is not None:
            raise ValueError("Duplicate build completion events cannot qualify")
        self.finished = finished

    def _record_summary(self, event):
        identity = event["id"]["testSummary"]
        label = identity["label"]
        if label in self.summaries:
            raise ValueError("Duplicate target summaries cannot qualify")
        self.summaries[label] = {**event["testSummary"],
                                 "configuration_id": identity["configuration"]["id"]}


def _read_invocation_events(bep_path):
    events = _InvocationEvents()
    with bep_path.open(encoding="utf-8") as stream:
        for line in stream:
            events.record(json.loads(line))
    return events


def _validate_invocation(events, expected):
    if events.finished is None:
        raise ValueError("The event stream does not record a successful completed build")
    exit_code = events.finished.get("exitCode", {})
    if exit_code.get("name") != "SUCCESS" or exit_code.get("code", 0) != 0:
        raise ValueError("The event stream does not record a successful completed build")
    if events.started is None or set(events.summaries) != expected:
        raise ValueError("Invocation did not complete every required test target")


def _summary_shard_count(summary):
    if (summary.get("overallStatus") != "PASSED" or
            summary.get("attemptCount", 1) != 1 or summary.get("runCount", 1) != 1):
        raise ValueError("Full validation requires one successful attempt per target")
    shard_count = max(1, summary.get("shardCount", 0))
    if summary.get("shardCount", 0) < 0 or summary.get("totalRunCount") != shard_count:
        raise ValueError("Test summary has inconsistent shard/run counts")
    return shard_count


def _expected_shards(shard_counts):
    return {(label, shard) for label, count in shard_counts.items()
            for shard in range(1, count + 1)}


def _validate_result_status(label, result, expected):
    if label not in expected or result.get("status") != "PASSED":
        raise ValueError("Unexpected, failed, or cached test result")
    execution = result.get("executionInfo", {})
    if any((result.get("cachedLocally"), result.get("cachedRemotely"),
            execution.get("cachedRemotely"))):
        raise ValueError("Unexpected, failed, or cached test result")


def _validate_result(identity, result, summaries, expected_identities, seen):
    label = identity["label"]
    _validate_result_status(label, result, summaries)
    key = (label, identity.get("shard", 1))
    if key not in expected_identities or key in seen:
        raise ValueError("Unexpected or duplicate test shard")
    if identity["configuration"]["id"] != summaries[label]["configuration_id"]:
        raise ValueError("Test result and summary configurations differ")
    if identity.get("attempt", 1) != 1 or identity.get("run", 1) != 1:
        raise ValueError("Repeated test attempts cannot qualify a receipt")
    seen.add(key)
    return key


def _test_output_directory(testlogs, label, shard, shard_count):
    package, name = label.removeprefix("//").split(":", 1)
    directory = testlogs / package / name
    if shard_count > 1:
        directory /= f"shard_{shard}_of_{shard_count}"
    return directory


def _test_output_uris(result):
    outputs = {}
    for item in result["testActionOutput"]:
        name = item["name"]
        if name in outputs:
            raise ValueError("Duplicate test outputs cannot qualify")
        outputs[name] = item["uri"]
    return outputs


def _validate_test_output(record, filename, data):
    if filename == "test.xml":
        record["cases"] = validate_xml(data)
    if filename == "test.log" and record["target"] == CAPABILITY:
        if parse_profile(data.decode("utf-8")) != "full":
            raise ValueError("The execution device did not prove full shader validation")


def _collect_test_record(testlogs, label, shard, shard_count, result, destination):
    directory = _test_output_directory(testlogs, label, shard, shard_count)
    name = label.split(":", 1)[1]
    outputs = _test_output_uris(result)
    record = {"target": label, "shard": shard,
              "strategy": result.get("executionInfo", {}).get("strategy", "unknown")}
    for filename in ("test.log", "test.xml"):
        data, digest = output_digest(outputs[filename], directory / filename)
        saved = f"{name}-{shard}-{filename}"
        (destination / saved).write_bytes(data)
        record[filename] = {**digest, "file": saved}
        _validate_test_output(record, filename, data)
    return record


def collect_results(bep_path, testlogs, expected, destination):
    events = _read_invocation_events(bep_path)
    _validate_invocation(events, set(expected))
    shard_counts = {label: _summary_shard_count(summary)
                    for label, summary in events.summaries.items()}
    expected_identities = _expected_shards(shard_counts)
    seen = set()
    records = []
    for identity, result in events.results:
        label, shard = _validate_result(identity, result, events.summaries, expected_identities, seen)
        records.append(_collect_test_record(testlogs, label, shard, shard_counts[label],
                                            result, destination))
    if seen != expected_identities:
        raise ValueError("A required shard has no fresh execution evidence")
    return {"invocation_id": events.started["uuid"],
            "bazel_version": events.started["buildToolVersion"],
            "configuration_ids": sorted(events.configurations), "tests": records}


def run_logged(command, root, output):
    with output.open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise ValueError(f"Verification command failed; inspect {output}")


def _parse_arguments():
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
    return args


def _prepare_artifacts(receipt):
    receipt.parent.mkdir(parents=True, exist_ok=True)
    if receipt.exists():
        raise ValueError("Refusing to replace an existing verification receipt")
    return Path(tempfile.mkdtemp(prefix=receipt.stem + "-", dir=receipt.parent))


def _full_validation_options(configs, jobs):
    common = [*configs, f"--jobs={jobs}", "--remote_local_fallback=false",
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
    return common


def _run_verification_phases(root, startup, common, artifacts):
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
    return invocations


def _toolchain_record(action):
    environment = {item["key"]: item["value"] for item in action.get("environmentVariables", [])}
    return {"mnemonic": action["mnemonic"], "action_key": action["actionKey"],
            "arguments_sha256": sha256(json.dumps(action.get("arguments", [])).encode()),
            "sdk_version": environment.get("APPLE_SDK_VERSION_OVERRIDE"),
            "xcode_version": environment.get("XCODE_VERSION_OVERRIDE")}


def _capture_toolchains(root, startup, configs, artifacts):
    toolchain_path = artifacts / "toolchain.json"
    query = f'mnemonic("ObjcCompile|CppCompile", deps({CAPABILITY}))'
    with toolchain_path.open("w", encoding="utf-8") as output, \
            (artifacts / "toolchain.log").open("w", encoding="utf-8") as error:
        subprocess.run([*startup, "aquery", *configs, "--output=jsonproto", query],
                       cwd=root, stdout=output, stderr=error, check=True)
    actions = json.loads(toolchain_path.read_text())["actions"]
    if not actions:
        raise ValueError("No compiler action was recorded for the native capability probe")
    return [_toolchain_record(action) for action in actions], toolchain_path


def _verify_inputs_unchanged(root, revision, rc_files, rc_hashes):
    if clean_revision(root) != revision:
        raise ValueError("Source revision changed during verification")
    if any(sha256(path.read_bytes()) != rc_hashes[str(path)] for path in rc_files):
        raise ValueError("Explicit configuration changed during verification")


def _dependency_hashes(root):
    dependencies = {}
    for name in (".bazelversion", "MODULE.bazel", "MODULE.bazel.lock", "tools/python/MODULE.bazel",
                 "tools/python/requirements.txt"):
        path = root / name
        if path.is_file():
            dependencies[name] = sha256(path.read_bytes())
    return dependencies


def _write_receipt(receipt, artifacts, document):
    temporary = artifacts / "receipt.json"
    temporary.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    os.chmod(temporary, 0o600)
    os.replace(temporary, receipt)


def main():
    args = _parse_arguments()
    root = Path(git(Path.cwd(), "rev-parse", "--show-toplevel"))
    revision = clean_revision(root)
    receipt = args.receipt.resolve()
    artifacts = _prepare_artifacts(receipt)
    rc_files = [path.resolve() for path in args.bazelrc]
    rc_hashes = {str(path): sha256(path.read_bytes()) for path in rc_files}
    startup = [args.bazel, "--nohome_rc", *("--bazelrc=" + str(path) for path in rc_files)]
    configs = ["--config=ci", *("--config=" + value for value in args.config)]
    common = _full_validation_options(configs, args.jobs)
    invocations = _run_verification_phases(root, startup, common, artifacts)
    toolchains, toolchain_path = _capture_toolchains(root, startup, configs, artifacts)
    _verify_inputs_unchanged(root, revision, rc_files, rc_hashes)
    document = {"schema": 1, "status": "passed", "source_revision": revision,
                "source_tree": git(root, "rev-parse", revision + "^{tree}"),
                "verified_at": datetime.now(timezone.utc).isoformat(), "profile": "full",
                "dependency_hashes": _dependency_hashes(root), "explicit_bazelrc_hashes": rc_hashes,
                "invocations": invocations,
                "toolchain_actions": toolchains, "toolchain_sha256": sha256(toolchain_path.read_bytes()),
                "local_artifacts": artifacts.name}
    _write_receipt(receipt, artifacts, document)
    print(f"Full Metal validation passed for {revision}; local receipt: {receipt}")

if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError, KeyError) as error:
        print(f"Metal validation did not qualify: {error}", file=sys.stderr)
        sys.exit(1)
