"""Reject stale, incomplete, skipped, or relaxed Metal validation evidence."""

import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import metal_validation_profile as profile
import verify_metal_validation as verifier


FULL = "METAL_VALIDATION_PROFILE profile=full validation_state=enabled"
NARROW = "METAL_VALIDATION_PROFILE profile=paravirtual-texture-usage-off validation_state=disabled"
PASSED_XML = b'<testsuites><testsuite tests="1"><testcase name="renders" status="run"/></testsuite></testsuites>'


class MetalXmlSecurityTest(unittest.TestCase):
    def test_valid_metadata_and_utf16_are_preserved(self):
        document = b'''<testsuites failures="0" errors="0" skipped="0" disabled="0">
          <testsuite name="native" tests="2" time="0.5">
            <properties><property name="renderer" value="safe &amp; bounded"/></properties>
            <testcase name="renders" status="run" result="completed" classname="Native"/>
            <testcase name="reads" time="0.1"/>
          </testsuite>
        </testsuites>'''
        self.assertEqual(2, verifier.validate_xml(document))
        self.assertEqual(2, verifier.validate_xml(document.decode().encode("utf-16")))

    def test_rejects_internal_dtd_and_entity_expansion(self):
        documents = (
            b'<!DOCTYPE testsuite><testsuite><testcase/></testsuite>',
            b'''<!DOCTYPE testsuite [<!ENTITY value "expanded">]>
                <testsuite><testcase name="&value;"/></testsuite>''',
            b'''<!DOCTYPE testsuite [
                <!ENTITY a "small">
                <!ENTITY b "&a;&a;&a;&a;">
                <!ENTITY c "&b;&b;&b;&b;">
                ]><testsuite><testcase name="&c;"/></testsuite>''',
        )
        for document in documents:
            with self.subTest(document=document), self.assertRaisesRegex(ValueError, "DTD"):
                verifier.validate_xml(document)

    def test_rejects_external_entities_and_parameter_entities(self):
        documents = (
            b'<!DOCTYPE testsuite SYSTEM "https://example.invalid/test.dtd">'
            b'<testsuite><testcase/></testsuite>',
            b'''<!DOCTYPE testsuite [
                <!ENTITY external SYSTEM "file:///untrusted-test-entity">
                ]><testsuite><testcase>&external;</testcase></testsuite>''',
            b'''<!DOCTYPE testsuite [
                <!ENTITY % external SYSTEM "https://example.invalid/entity.dtd">
                %external;
                ]><testsuite><testcase/></testsuite>''',
        )
        for document in documents:
            with self.subTest(document=document), self.assertRaisesRegex(ValueError, "DTD"):
                verifier.validate_xml(document)

    def test_rejects_utf16_doctype_before_elements_are_processed(self):
        document = ('<?xml version="1.0" encoding="UTF-16"?>'
                    '<!DOCTYPE testsuite [<!ENTITY value "expanded">]>'
                    '<testsuite><testcase name="&value;"/></testsuite>').encode("utf-16")
        with mock.patch.object(verifier._TestXmlReader, "element_started") as started:
            with self.assertRaisesRegex(ValueError, "DTD"):
                verifier.validate_xml(document)
        started.assert_not_called()

    def test_rejects_malformed_xml(self):
        for document in (b"", b"<testsuite>", b"<testcase/><testcase/>",
                         b"<testsuite><testcase></testsuite>", b"<testcase>&unknown;</testcase>"):
            with self.subTest(document=document), self.assertRaisesRegex(ValueError, "Malformed"):
                verifier.validate_xml(document)

    def test_old_expat_is_rejected_before_parser_creation(self):
        for version in ((1, 95, 8), (2, 5, 0)):
            with self.subTest(version=version), \
                    mock.patch.object(verifier.expat, "version_info", version), \
                    mock.patch.object(verifier.expat, "ParserCreate") as create:
                with self.assertRaisesRegex(ValueError, "Expat 2.6"):
                    verifier.validate_xml(PASSED_XML)
                create.assert_not_called()

    def test_xml_byte_limit_applies_before_parser_creation(self):
        with mock.patch.object(verifier.expat, "ParserCreate") as create:
            with self.assertRaisesRegex(ValueError, "byte limit"):
                verifier.validate_xml(b" " * (verifier.MAX_XML_BYTES + 1))
            create.assert_not_called()

    def test_depth_limit_includes_the_testcase(self):
        def nested_document(depth):
            return b"<group>" * (depth - 1) + b"<testcase/>" + b"</group>" * (depth - 1)

        self.assertEqual(1, verifier.validate_xml(nested_document(verifier.MAX_XML_DEPTH)))
        with self.assertRaisesRegex(ValueError, "depth limit"):
            verifier.validate_xml(nested_document(verifier.MAX_XML_DEPTH + 1))

    def test_element_limit_counts_all_metadata_elements(self):
        def document(count):
            return (b"<testsuite><testcase/>" + b"<property/>" * (count - 2) + b"</testsuite>")

        self.assertEqual(1, verifier.validate_xml(document(verifier.MAX_XML_ELEMENTS)))
        with self.assertRaisesRegex(ValueError, "element limit"):
            verifier.validate_xml(document(verifier.MAX_XML_ELEMENTS + 1))

    def test_attribute_limit_is_enforced_per_element(self):
        def document(count):
            attributes = " ".join(f'a{index}="value"' for index in range(count))
            return f"<testcase {attributes}/>".encode()

        self.assertEqual(1, verifier.validate_xml(document(verifier.MAX_XML_ATTRIBUTES)))
        with self.assertRaisesRegex(ValueError, "attribute limit"):
            verifier.validate_xml(document(verifier.MAX_XML_ATTRIBUTES + 1))

    def test_namespace_declarations_share_the_attribute_limit(self):
        def document(count, extra_attribute=""):
            namespaces = " ".join(f'xmlns:n{index}="urn:{index}"' for index in range(count))
            return f"<testsuite {namespaces} {extra_attribute}><testcase/></testsuite>".encode()

        self.assertEqual(1, verifier.validate_xml(document(verifier.MAX_XML_ATTRIBUTES)))
        for data in (document(verifier.MAX_XML_ATTRIBUTES + 1),
                     document(verifier.MAX_XML_ATTRIBUTES, 'name="native"')):
            with self.subTest(data=data), self.assertRaisesRegex(ValueError, "attribute limit"):
                verifier.validate_xml(data)

    def test_namespace_uri_limit_precedes_qualified_elements(self):
        limit = verifier.MAX_XML_NAMESPACE_URI_CHARS
        allowed_uri = "urn:" + "u" * (limit - 4)
        allowed = f'<testsuite xmlns:n="{allowed_uri}"><n:testcase/></testsuite>'.encode()
        self.assertEqual(1, verifier.validate_xml(allowed))
        oversized_uri = allowed_uri + "u"
        references = "<n:p/>" * 2000
        hostile = (f'<testsuite xmlns:n="{oversized_uri}"><testcase/>'
                   f'{references}</testsuite>').encode()
        with self.assertRaisesRegex(ValueError, "namespace URI limit"):
            verifier.validate_xml(hostile)
        with mock.patch.object(verifier._TestXmlReader, "element_started") as started:
            with self.assertRaisesRegex(ValueError, "namespace URI limit"):
                verifier.validate_xml(hostile)
            started.assert_not_called()

    def test_namespace_prefix_limit_accepts_the_boundary(self):
        prefix = "p" * verifier.MAX_XML_NAMESPACE_PREFIX_CHARS
        allowed = (f'<testsuite xmlns:{prefix}="urn:fixture">'
                   f'<{prefix}:testcase/></testsuite>').encode()
        self.assertEqual(1, verifier.validate_xml(allowed))
        prefix += "p"
        oversized = (f'<testsuite xmlns:{prefix}="urn:fixture">'
                     f'<{prefix}:testcase/></testsuite>').encode()
        with self.assertRaisesRegex(ValueError, "namespace prefix limit"):
            verifier.validate_xml(oversized)

    def test_default_namespace_uri_has_the_same_limit(self):
        uri = "urn:" + "u" * (verifier.MAX_XML_NAMESPACE_URI_CHARS - 4)
        self.assertEqual(1, verifier.validate_xml(f'<testcase xmlns="{uri}"/>'.encode()))
        with self.assertRaisesRegex(ValueError, "namespace URI limit"):
            verifier.validate_xml(f'<testcase xmlns="{uri}u"/>'.encode())

    def test_single_expanded_qname_limit_covers_elements_and_attributes(self):
        uri = "urn:" + "u" * (verifier.MAX_XML_NAMESPACE_URI_CHARS - 4)
        local_length = verifier.MAX_XML_QNAME_CHARS - len(uri) - 1

        def document(kind, length):
            local = "a" * length
            if kind == "element":
                content = f'<n:{local}/><testcase/>'
            else:
                content = f'<testcase n:{local}="value"/>'
            return f'<testsuite xmlns:n="{uri}">{content}</testsuite>'.encode()

        for kind in ("element", "attribute"):
            with self.subTest(kind=kind):
                self.assertEqual(1, verifier.validate_xml(document(kind, local_length)))
                with self.assertRaisesRegex(ValueError, "expanded QName limit"):
                    verifier.validate_xml(document(kind, local_length + 1))

    def test_repeated_namespace_references_obey_the_exact_expanded_name_budget(self):
        uri = "urn:" + "u" * (verifier.MAX_XML_NAMESPACE_URI_CHARS - 4)
        overhead = len("testsuite") + len("testcase")
        for node, name_chars in (("<n:p/>", len(uri) + 2),
                                 ('<p n:a="value"/>', len(uri) + 3)):
            with self.subTest(node_kind="attribute" if "=" in node else "element"):
                count, remainder = divmod(verifier.MAX_XML_EXPANDED_NAME_CHARS - overhead,
                                          name_chars)
                prefix = f'<testsuite xmlns:n="{uri}"><testcase/>' + node * count
                filler = "f" * remainder
                exact = f'{prefix}<{filler}/></testsuite>'.encode()
                self.assertEqual(1, verifier.validate_xml(exact))
                oversized = f'{prefix}<{filler}f/></testsuite>'.encode()
                with self.assertRaisesRegex(ValueError, "expanded name budget"):
                    verifier.validate_xml(oversized)

    def test_case_status_and_failure_metadata_cannot_hide_in_namespaces(self):
        documents = (
            b'<testsuite xmlns:m="urn:m"><testcase><m:failure/></testcase></testsuite>',
            b'<testsuite xmlns:m="urn:m" m:disabled="1"><testcase/></testsuite>',
            b'<testcase xmlns:m="urn:m" m:status="notrun"/>',
            b'<testcase xmlns:m="urn:m" m:result="suppressed"/>',
            b'<testsuite><testcase><disabled/></testcase></testsuite>',
            b'<testcase result="skipped"/>', b'<testcase status="disabled"/>',
            b'<testcase status="running"/>', b'<testcase result="pending"/>',
        )
        for document in documents:
            with self.subTest(document=document), self.assertRaises(ValueError):
                verifier.validate_xml(document)


class MetalEvidenceTest(unittest.TestCase):
    def test_profile_requires_one_consistent_marker(self):
        self.assertEqual("full", profile.parse_profile("test wrapper\n" + FULL + "\n"))
        self.assertEqual("paravirtual-texture-usage-off", profile.parse_profile(NARROW))
        for text in ("", FULL + "\n" + FULL, FULL + "\n" + NARROW,
                     FULL.replace("enabled", "disabled"), FULL + " trailing", " " + FULL):
            with self.subTest(text=text), self.assertRaises(ValueError):
                profile.parse_profile(text)

    def test_xml_requires_executed_cases_without_skips_or_failures(self):
        self.assertEqual(1, verifier.validate_xml(PASSED_XML))
        for data in (b'<testsuites/>', b'<testsuite><testcase><skipped/></testcase></testsuite>',
                     b'<testsuite failures="1"><testcase/></testsuite>',
                     b'<testsuite disabled="1"><testcase/></testsuite>',
                     b'<testsuite><testcase status="notrun"/></testsuite>',
                     b'<testsuite><testcase><error/></testcase></testsuite>'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                verifier.validate_xml(data)

    def test_remote_output_must_match_digest_and_size(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.log"
            path.write_bytes(b"current output")
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            uri = f"bytestream://example.invalid/blobs/{digest}/{path.stat().st_size}"
            self.assertEqual(b"current output", verifier.output_digest(uri, path)[0])
            path.write_bytes(b"stale output")
            with self.assertRaises(ValueError):
                verifier.output_digest(uri, path)
            with self.assertRaises(ValueError):
                verifier.output_digest("https://example.invalid/test.log", path)

    def test_dirty_source_cannot_qualify(self):
        with mock.patch.object(verifier, "git", return_value=" M changed.cc"):
            with self.assertRaises(ValueError):
                verifier.clean_revision(Path("."))

    def collect_fixture(self, marker=FULL, status="PASSED", cached=False, include_summary=True,
                        shard_count=1, result_shards=None, cached_remote=False,
                        summary_configuration="test-config", finished=True, mutate_events=None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            logs = root / "testlogs"
            directory = logs / "donner/gpu/metal/tests/metal_full_validation_required"
            label = verifier.CAPABILITY
            events = [{"started": {"uuid": "test-invocation", "buildToolVersion": "8"}}]
            if result_shards is None:
                result_shards = list(range(1, shard_count + 1))
            for shard in result_shards:
                shard_directory = directory
                if shard_count > 1:
                    shard_directory /= f"shard_{shard}_of_{shard_count}"
                shard_directory.mkdir(parents=True, exist_ok=True)
                (shard_directory / "test.log").write_text(marker + "\n")
                (shard_directory / "test.xml").write_bytes(PASSED_XML)
                events.append({"id": {"testResult": {"label": label, "shard": shard,
                                                      "configuration": {"id": "test-config"}}},
                               "testResult": {"status": status, "cachedLocally": cached,
                                              "executionInfo": {"cachedRemotely": cached_remote},
                                              "testActionOutput": [
                                                  {"name": name, "uri": (shard_directory / name).as_uri()}
                                                  for name in ("test.log", "test.xml")]}})
            if include_summary:
                events.append({"id": {"testSummary": {"label": label,
                                                       "configuration": {"id": summary_configuration}}},
                               "testSummary": {"overallStatus": "PASSED", "shardCount": shard_count,
                                               "totalRunCount": shard_count}})
            if finished:
                events.append({"finished": {"exitCode": {"name": "SUCCESS"}}})
            if mutate_events is not None:
                mutate_events(events)
            bep = root / "events.json"
            bep.write_text("\n".join(json.dumps(event) for event in events))
            output = root / "evidence"
            output.mkdir()
            return verifier.collect_results(bep, logs, (label,), output)

    def test_every_uncached_shard_and_matching_configuration_is_required(self):
        self.assertEqual(2, len(self.collect_fixture(shard_count=2)["tests"]))
        for kwargs in ({"shard_count": 2, "result_shards": [1]},
                       {"result_shards": [1, 1]}, {"result_shards": [2]},
                       {"cached_remote": True}, {"finished": False},
                       {"summary_configuration": "different"}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.collect_fixture(**kwargs)

    def test_capability_receipt_accepts_only_full_fresh_complete_evidence(self):
        self.assertEqual(1, self.collect_fixture()["tests"][0]["cases"])
        for kwargs in ({"marker": NARROW}, {"marker": ""}, {"status": "FAILED"},
                       {"cached": True}, {"include_summary": False}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.collect_fixture(**kwargs)


    def test_receipt_fields_and_digests_are_preserved(self):
        evidence = self.collect_fixture()
        self.assertEqual("test-invocation", evidence["invocation_id"])
        self.assertEqual("8", evidence["bazel_version"])
        self.assertEqual(["test-config"], evidence["configuration_ids"])
        self.assertEqual([{
            "target": verifier.CAPABILITY, "shard": 1, "strategy": "unknown", "cases": 1,
            "test.log": {"sha256": verifier.sha256((FULL + "\n").encode()),
                         "size": len(FULL) + 1, "file": "metal_full_validation_required-1-test.log"},
            "test.xml": {"sha256": verifier.sha256(PASSED_XML), "size": len(PASSED_XML),
                         "file": "metal_full_validation_required-1-test.xml"},
        }], evidence["tests"])

    def test_duplicate_invocation_boundaries_and_summaries_are_rejected(self):
        for kind in ("started", "finished", "testSummary"):
            def duplicate(events):
                events.append(next(event for event in events if kind in event))

            with self.subTest(kind=kind), self.assertRaises(ValueError):
                self.collect_fixture(mutate_events=duplicate)

    def test_summary_requires_one_successful_attempt_and_consistent_shards(self):
        changes = ({"overallStatus": "FAILED"}, {"attemptCount": 2}, {"runCount": 2},
                   {"shardCount": -1}, {"totalRunCount": 2})
        for fields in changes:
            def mutate(events):
                summary = next(event["testSummary"] for event in events if "testSummary" in event)
                summary.update(fields)

            with self.subTest(fields=fields), self.assertRaises(ValueError):
                self.collect_fixture(mutate_events=mutate)

    def test_failed_completion_cannot_qualify(self):
        for exit_code in ({"name": "FAILURE", "code": 0}, {"name": "SUCCESS", "code": 1}):
            def mutate(events):
                events[-1]["finished"]["exitCode"] = exit_code

            with self.subTest(exit_code=exit_code), self.assertRaises(ValueError):
                self.collect_fixture(mutate_events=mutate)

    def test_repeated_attempts_and_runs_cannot_qualify(self):
        for field in ("attempt", "run"):
            def mutate(events):
                identity = next(event["id"]["testResult"] for event in events if "testResult" in event)
                identity[field] = 2

            with self.subTest(field=field), self.assertRaises(ValueError):
                self.collect_fixture(mutate_events=mutate)

    def test_duplicate_output_names_cannot_qualify(self):
        def mutate(events):
            result = next(event["testResult"] for event in events if "testResult" in event)
            result["testActionOutput"].append(dict(result["testActionOutput"][0]))

        with self.assertRaisesRegex(ValueError, "Duplicate test outputs"):
            self.collect_fixture(mutate_events=mutate)

    def test_top_level_remote_cache_and_unexpected_targets_cannot_qualify(self):
        def cache_result(events):
            result = next(event["testResult"] for event in events if "testResult" in event)
            result["cachedRemotely"] = True

        def change_label(events):
            identity = next(event["id"]["testResult"] for event in events if "testResult" in event)
            identity["label"] = verifier.PACKAGE + "unexpected"

        for mutate in (cache_result, change_label):
            with self.subTest(mutate=mutate.__name__), self.assertRaises(ValueError):
                self.collect_fixture(mutate_events=mutate)

    def test_growing_outputs_cannot_bypass_the_read_bound(self):
        path = mock.Mock(spec=Path)
        path.stat.return_value.st_size = 1
        context = mock.MagicMock()
        stream = context.__enter__.return_value
        stream.read.return_value = b"12345"
        path.open.return_value = context
        uri = "bytestream://example.invalid/blobs/" + "0" * 64 + "/1"
        with mock.patch.object(verifier, "MAX_LOG_BYTES", 4):
            with self.assertRaisesRegex(ValueError, "size bound"):
                verifier.output_digest(uri, path)
        stream.read.assert_called_once_with(5)

    def test_remote_output_requires_correct_size_and_local_uri_requires_same_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.log"
            path.write_bytes(b"current")
            uri = f"bytestream://example.invalid/blobs/{verifier.sha256(b'current')}/999"
            with self.assertRaises(ValueError):
                verifier.output_digest(uri, path)
            with self.assertRaises(ValueError):
                verifier.output_digest((path.parent / "different.log").as_uri(), path)


    def test_full_validation_options_preserve_the_required_execution_policy(self):
        self.assertEqual([
            "--config=ci", "--config=re", "--jobs=4", "--remote_local_fallback=false",
            "--test_tag_filters=", "--test_filter=", "--runs_per_test=1",
            "--flaky_test_attempts=1", "--nocache_test_results", "--test_output=all",
            "--test_env=MTL_DEBUG_LAYER=1", "--test_env=MTL_SHADER_VALIDATION=1",
            "--test_env=MTL_SHADER_VALIDATION_TEXTURE_USAGE=1",
            "--test_env=MTL_SHADER_VALIDATION_GLOBAL_MEMORY=1",
            "--test_env=MTL_SHADER_VALIDATION_THREADGROUP_MEMORY=1",
            "--test_env=MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING=1",
            "--test_env=MTL_SHADER_VALIDATION_ABORT_ON_FAULT=1",
            "--test_env=MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1",
            "--test_env=DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER=1",
            "--test_env=MTL_SHADER_VALIDATION_DEFAULT_STATE=all",
            "--test_env=MTL_SHADER_VALIDATION_DISABLE_PIPELINES=",
            "--test_env=MTL_SHADER_VALIDATION_ENABLE_PIPELINES=",
        ], verifier._full_validation_options(["--config=ci", "--config=re"], 4))

    def test_source_and_explicit_configuration_changes_still_block_receipts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rc = root / "fixture.bazelrc"
            rc.write_bytes(b"original configuration")
            hashes = {str(rc): verifier.sha256(rc.read_bytes())}
            with mock.patch.object(verifier, "clean_revision", return_value="changed"):
                with self.assertRaisesRegex(ValueError, "Source revision changed"):
                    verifier._verify_inputs_unchanged(root, "original", [rc], hashes)
            rc.write_bytes(b"changed configuration")
            with mock.patch.object(verifier, "clean_revision", return_value="original"):
                with self.assertRaisesRegex(ValueError, "Explicit configuration changed"):
                    verifier._verify_inputs_unchanged(root, "original", [rc], hashes)


if __name__ == "__main__":
    unittest.main()
