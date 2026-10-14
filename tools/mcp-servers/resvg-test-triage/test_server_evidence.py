"""Exercise real server handlers with transport-only stubs, without installing MCP."""

import asyncio
import importlib.util
import json
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

from test_triage_evidence import png


class TransportServer:
    """Only the decorator surface used while registering tool handlers."""

    def __init__(self, name):
        self.name = name

    def list_tools(self):
        return lambda function: function

    def call_tool(self):
        return lambda function: function


def load_handler_module():
    mcp = types.ModuleType("mcp")
    server_module = types.ModuleType("mcp.server")
    server_module.Server = TransportServer
    content_module = types.ModuleType("mcp.types")
    for name in ("Tool", "TextContent", "ImageContent"):
        setattr(content_module, name, types.SimpleNamespace)
    spec = importlib.util.spec_from_file_location(
        "triage_server_under_test", Path(__file__).with_name("server.py")
    )
    module = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {"mcp": mcp, "mcp.server": server_module,
                                  "mcp.types": content_module, spec.name: module}):
        with patch.dict(os.environ, {}, clear=True):
            spec.loader.exec_module(module)
    return module


class ServerEvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = load_handler_module()

    def call(self, name, arguments):
        response = asyncio.run(self.server.call_tool(name, arguments))
        return json.loads(response[0].text)

    def test_feature_presence_does_not_prove_unsupported(self):
        for count in (1, 100, 500, 999, 10000):
            result = self.call("analyze_test_failure", {
                "test_name": "spacing.svg", "pixel_diff": count,
                "svg_content": '<svg><text letter-spacing="10">text</text></svg>',
            })
            self.assertEqual(result["category"], "unresolved")
            self.assertEqual(result["severity"], "unknown")
            self.assertIsNone(result["suggested_skip"])
            self.assertEqual(result["features"][0]["name"], "letter-spacing")

    def test_feature_observation_order_and_deduplication(self):
        result = self.call("detect_svg_features", {"svg_content":
            '<svg><text x="1 2" dx="2" style="font-style:italic;font-variant:small-caps">'
            '<tspan dx="1">text</tspan></text></svg>'})
        self.assertEqual([feature["name"] for feature in result["features"]],
                         ["multiple_x_values", "dx_attribute", "tspan", "font-style", "font-variant"])

    def test_all_tool_handlers_are_registered(self):
        advertised = {tool.name for tool in asyncio.run(self.server.list_tools())}
        self.assertEqual(set(self.server._TOOL_HANDLERS), advertised)
        with self.assertRaisesRegex(ValueError, "Unknown tool"):
            self.call("not_a_tool", {})

    def test_invalid_xml_and_entities_fail_closed(self):
        for source in ('<svg>', '<!DOCTYPE svg><svg/>',
                       '<!DOCTYPE svg [<!ENTITY x "xx">]><svg>&x;</svg>'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                self.call("detect_svg_features", {"svg_content": source})

    def test_default_image_access_is_denied_by_handler(self):
        with self.assertRaisesRegex(ValueError, "requires"):
            self.call("analyze_test_failure", {
                "test_name": "x.svg", "pixel_diff": 1, "svg_content": "<svg/>",
                "actual_image_path": "/unrelated/image.png",
            })

    def test_skip_is_proposal_and_escapes_untrusted_name(self):
        result = self.call("suggest_skip_comment", {
            "test_name": 'untrusted"\\name.svg', "category": "font_difference",
        })
        self.assertEqual((result["proposal_only"], result["requires_human_review"],
                          result["applied"]), (True, True, False))
        self.assertIn('untrusted\\"\\\\name.svg', result["skip_comment"])
        self.assertIn("requires causal evidence", result["skip_comment"])
        self.assertNotIn("CoreText", result["skip_comment"])

    def test_batch_keeps_crashed_and_incomplete_cases(self):
        result = self.call("batch_triage_tests", {"test_output":
            "[ RUN ] SvgSuite.a\n[ OK ] SvgSuite.a\n[ RUN ] SvgSuite.b\nSegmentation fault\n"})
        self.assertEqual(result["total_failures"], 1)
        self.assertEqual(result["failures"][0]["status"], "INCOMPLETE")
        self.assertEqual(result["run_complete"], False)
        self.assertEqual(result["incomplete_tests"], 1)
        self.assertIn("Segmentation fault", result["failures"][0]["raw_output"])

    def test_batch_keeps_quiet_failures_without_svg_dump(self):
        result = self.call("batch_triage_tests", {"test_output": """[ RUN      ] Suite.Test/case
renderer failed before compare
[  FAILED  ] Suite.Test/case
"""})
        self.assertEqual(result["summary"], {"unresolved": 1})
        self.assertIsNone(result["failures"][0]["pixel_diff"])
        self.assertIsNone(result["failures"][0]["suggested_skip"])
        self.assertIn("renderer failed", result["failures"][0]["raw_output"])

    def test_visual_handler_reports_supplied_metric_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            for name in ("actual", "expected", "diff"):
                (root / f"{name}.png").write_bytes(png())
            arguments = {f"{name}_image_path": f"{name}.png"
                         for name in ("actual", "expected", "diff")}
            with patch.object(self.server, "ARTIFACT_ROOT", str(root)):
                result = self.call("analyze_visual_diff", arguments)
                self.assertIsNone(result["visual_analysis"]["diff_pixels"])
                result = self.call("analyze_visual_diff", {**arguments, "pixel_diff": 0})
                self.assertEqual(result["visual_analysis"]["diff_pixels"], 0)
                self.assertEqual(result["difference_type"], "unresolved")

    def test_no_declared_skips_does_not_claim_feature_implementation(self):
        result = self.call("generate_feature_report", {
            "category": "painting/fill", "skip_file_content": "// no declared skips",
            "test_output": """[ RUN      ] Suite.Test/red
[  COMPARE ] /tests/painting/fill/red.svg [TinySkia]: FAIL (1 pixels differ)
[  FAILED  ] Suite.Test/red
""",
        })
        self.assertEqual(result["failing"], 1)
        self.assertNotIn("all features implemented", result["next_priority"])


if __name__ == "__main__":
    unittest.main()
