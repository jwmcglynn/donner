#!/usr/bin/env python3
"""Adapter-contract tests: request/response protocol and status handling."""

from __future__ import annotations

import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import adapter_protocol
import runner
import suite_test_support as support


class ParseResponseTest(unittest.TestCase):
    def test_valid_ok_response(self):
        response = adapter_protocol.parse_response(
            '{"status": "ok", "width": 4, "height": 4, "format": "rgba8"}'
        )
        self.assertEqual((response.status, response.width, response.height), ("ok", 4, 4))

    def test_unsupported_response(self):
        response = adapter_protocol.parse_response('{"status": "unsupported"}')
        self.assertEqual(response.status, "unsupported")

    def test_malformed_json_rejected(self):
        with self.assertRaises(adapter_protocol.AdapterProtocolError):
            adapter_protocol.parse_response("{ not json")

    def test_missing_status_rejected(self):
        with self.assertRaises(adapter_protocol.AdapterProtocolError):
            adapter_protocol.parse_response('{"width": 4}')

    def test_unknown_status_rejected(self):
        with self.assertRaises(adapter_protocol.AdapterProtocolError):
            adapter_protocol.parse_response('{"status": "great"}')

    def test_ok_without_dimensions_rejected(self):
        with self.assertRaises(adapter_protocol.AdapterProtocolError):
            adapter_protocol.parse_response('{"status": "ok", "format": "rgba8"}')

    def test_ok_with_wrong_format_rejected(self):
        with self.assertRaises(adapter_protocol.AdapterProtocolError):
            adapter_protocol.parse_response('{"status": "ok", "width": 4, "height": 4, "format": "gray8"}')


class AdapterStatusThroughRunnerTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.work = self.root / "work"

    def tearDown(self):
        self.temporary_directory.cleanup()

    def _run_single(self, entry, timeout=30.0, work_dir=None):
        manifest_path = support.write_manifest(self.root, [entry])
        return runner.run_manifest(
            manifest_path,
            support.adapter_argv(),
            work_dir=work_dir or self.work,
            timeout=timeout,
        )

    def test_unsupported_from_adapter(self):
        support.make_png(self.root / "tests/base.png")
        support.make_png(self.root / "tests/base.oracle.png")
        entry = support.test_entry(
            test_id="donner-svg2/u",
            input_rel="tests/base.png",
            oracle_rel="tests/base.oracle.png",
            capabilities=["unsupported"],
        )
        run = self._run_single(entry)
        self.assertEqual(run.results[0]["status"], "unsupported")
        self.assertTrue(run.ok)

    def test_crash_is_adapter_error(self):
        support.make_png(self.root / "tests/crash.png")
        support.make_png(self.root / "tests/crash.oracle.png")
        entry = support.test_entry(
            test_id="donner-svg2/c", input_rel="tests/crash.png", oracle_rel="tests/crash.oracle.png"
        )
        run = self._run_single(entry)
        self.assertEqual(run.results[0]["status"], "adapter-error")
        self.assertFalse(run.ok)

    def test_structured_error_is_adapter_error(self):
        support.make_png(self.root / "tests/reperror.png")
        support.make_png(self.root / "tests/reperror.oracle.png")
        entry = support.test_entry(
            test_id="donner-svg2/e", input_rel="tests/reperror.png", oracle_rel="tests/reperror.oracle.png"
        )
        run = self._run_single(entry)
        self.assertEqual(run.results[0]["status"], "adapter-error")

    def test_malformed_response_is_adapter_error(self):
        support.make_png(self.root / "tests/malformed.png")
        support.make_png(self.root / "tests/malformed.oracle.png")
        entry = support.test_entry(
            test_id="donner-svg2/mal", input_rel="tests/malformed.png", oracle_rel="tests/malformed.oracle.png"
        )
        run = self._run_single(entry)
        self.assertEqual(run.results[0]["status"], "adapter-error")

    def test_timeout(self):
        support.make_png(self.root / "tests/hang.png")
        support.make_png(self.root / "tests/hang.oracle.png")
        entry = support.test_entry(
            test_id="donner-svg2/h", input_rel="tests/hang.png", oracle_rel="tests/hang.oracle.png"
        )
        run = self._run_single(entry, timeout=0.5)
        self.assertEqual(run.results[0]["status"], "timeout")
        self.assertFalse(run.ok)

    def test_wrong_output_dimensions_is_adapter_error(self):
        # Input is 2x2 but the oracle declares 4x4, so the requested dimensions
        # do not match what the (echoing) adapter produces.
        support.make_png(self.root / "tests/dims.png", width=2, height=2)
        support.make_png(self.root / "tests/dims.oracle.png", width=4, height=4)
        entry = support.test_entry(
            test_id="donner-svg2/d",
            input_rel="tests/dims.png",
            oracle_rel="tests/dims.oracle.png",
            width=4,
            height=4,
        )
        run = self._run_single(entry)
        self.assertEqual(run.results[0]["status"], "adapter-error")
        self.assertIn("dimensions", run.results[0]["diagnostics"])

    def test_render_output_is_deterministic(self):
        support.make_png(self.root / "tests/base.png")
        support.make_png(self.root / "tests/base.oracle.png")
        entry = support.test_entry(
            test_id="donner-svg2/det", input_rel="tests/base.png", oracle_rel="tests/base.oracle.png"
        )
        sandbox_name = hashlib.sha256(b"donner-svg2/det").hexdigest()[:16]

        work_a = self.root / "work_a"
        work_b = self.root / "work_b"
        self._run_single(entry, work_dir=work_a)
        self._run_single(entry, work_dir=work_b)
        output_a = (work_a / sandbox_name / "out.png").read_bytes()
        output_b = (work_b / sandbox_name / "out.png").read_bytes()
        self.assertEqual(output_a, output_b)


if __name__ == "__main__":
    unittest.main()
