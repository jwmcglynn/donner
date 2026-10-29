"""CPU-only startup and relocation checks for the real editor helper executables."""

import json
import unittest

from donner.svg.renderer.tests.native_resource_delivery_fixture import (
    DeliveryChecks, NativeResourceFixture,
)


class ReplDelivery(NativeResourceFixture, DeliveryChecks):
    consumer = ("donner/editor/app", "render_repl", "render_repl_impl")

    def invoke(self):
        return self.run_binary(input="quit\n")

    def assert_resource_error(self, result):
        super().assert_resource_error(result)
        self.assertNotIn("type 'help'", result.stdout)

    def render(self, source, output):
        destination = self.cwd / output
        result = self.run_binary(input=f"resize 128 64\nload {source}\nsave {destination}\nquit\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("save: wrote ", result.stdout)
        return destination

    def test_relocated_fallback_produces_glyph_pixels(self):
        self.assert_glyph_pixels()

    def test_public_wrapper_has_an_adjacent_authenticated_companion(self):
        result = self.run_binary(input="quit\n", public_wrapper=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("type 'help'", result.stdout)


class ReplayDelivery(NativeResourceFixture, DeliveryChecks):
    consumer = ("donner/editor/tests", "editor_rnr_gl_replay", "editor_rnr_gl_replay_impl")

    def invoke(self, public_wrapper=False):
        return self.run_binary(["--rnr", self.cwd / "absent.rnr", "--capture-frame", "1"],
                               public_wrapper=public_wrapper)

    def assert_replay_input_error(self, result):
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("failed to read .rnr file:", result.stderr)
        self.assertNotIn("Packaged resources unavailable:", result.stderr)
        self.record_verified_registry()

    def test_relocated_registry_passes_before_existing_replay_input_error(self):
        self.install_assets()
        self.assert_replay_input_error(self.invoke())

    def test_public_wrapper_has_an_adjacent_authenticated_companion(self):
        self.assert_replay_input_error(self.invoke(public_wrapper=True))


class McpDelivery(NativeResourceFixture, DeliveryChecks):
    consumer = ("tools/mcp-servers/editor-control", "editor_control_mcp_server", "editor_control_mcp_server_impl")

    def invoke(self):
        return self.run_binary()

    def assert_resource_error(self, result):
        super().assert_resource_error(result)
        self.assertEqual(result.stdout, "")

    def test_relocated_registry_passes_without_writing_protocol_noise(self):
        self.install_assets()
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")
        self.record_verified_registry()

    def test_public_wrapper_has_an_adjacent_authenticated_companion(self):
        result = self.run_binary(public_wrapper=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")

    def test_relocated_startup_preserves_framed_json_rpc(self):
        self.install_assets()
        body = json.dumps({"jsonrpc": "2.0", "id": 7, "method": "initialize"})
        result = self.run_binary(input=f"Content-Length: {len(body)}\r\n\r\n" + body)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        headers, response = result.stdout.split("\n\n", 1)
        self.assertEqual(headers, "Content-Length: " + str(len(response)))
        message = json.loads(response)
        self.assertEqual(message["jsonrpc"], "2.0", message)
        self.assertEqual(message["id"], 7, message)
        self.assertEqual(message["result"]["serverInfo"]["name"], "donner-editor-control", message)


if __name__ == "__main__":
    unittest.main()
