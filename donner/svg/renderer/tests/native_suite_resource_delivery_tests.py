"""Offline text and fail-closed deployment checks for the actual native suite entry points."""

import json
import unittest

from donner.svg.renderer.tests.native_resource_delivery_fixture import (
    DeliveryChecks, NativeResourceFixture,
)


class SuiteRendererDelivery(NativeResourceFixture, DeliveryChecks):
    consumer = ("donner/svg/renderer/tests", "donner_svg_suite_renderer", "donner_svg_suite_renderer")

    def invoke(self):
        return self.run_binary(["--input", self.svg("input", True), "--root", self.cwd,
                                "--output", self.cwd / "output.png", "--width", "128", "--height", "64"])

    def render(self, source, output):
        destination = self.cwd / output
        result = self.run_binary(["--input", source, "--root", self.cwd, "--output", destination,
                                  "--width", "128", "--height", "64"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return destination

    def test_relocated_fallback_produces_glyph_pixels(self):
        self.assert_glyph_pixels()


class AdapterDelivery(NativeResourceFixture, DeliveryChecks):
    consumer = ("donner/svg/renderer/tests", "donner_svg2_render_adapter", "donner_svg2_render_adapter")

    def invoke(self):
        request = self.cwd / "request.json"
        request.write_text(json.dumps({"input": str(self.svg("input", True)),
                                       "output": str(self.cwd / "output.png"),
                                       "width": 128, "height": 64}))
        self.response = self.cwd / "response.json"
        return self.run_binary(["render", "--request", request, "--response", self.response])

    def diagnostics(self, result):
        return super().diagnostics(result) + (self.response.read_text() if self.response.exists() else "")

    def render(self, source, output):
        destination = self.cwd / output
        request = self.cwd / (output + ".json")
        response = self.cwd / (output + ".response.json")
        request.write_text(json.dumps({"input": str(source), "output": str(destination),
                                       "width": 128, "height": 64}))
        result = self.run_binary(["render", "--request", request, "--response", response])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(response.read_text())["status"], "ok")
        return destination

    def test_relocated_fallback_produces_glyph_pixels(self):
        self.assert_glyph_pixels()


if __name__ == "__main__":
    unittest.main()
