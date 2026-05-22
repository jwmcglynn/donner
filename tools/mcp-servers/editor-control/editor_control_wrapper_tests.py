#!/usr/bin/env python3
"""Tests for the editor-control MCP wrapper."""

from __future__ import annotations

import io
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import editor_control_wrapper


class FakeProxy:
  def __init__(self) -> None:
    self.restarted = False

  def state(self):
    return {
        "child_running": False,
        "last_build": None,
    }

  def restart_child(self, rebuild, timeout_ms):
    self.restarted = True
    return (
        True,
        {
            "restarted": True,
            "rebuild": rebuild,
            "timeout_ms": timeout_ms,
            "state": self.state(),
        },
    )


class FakeRebuildProxy(FakeProxy):
  def __init__(self) -> None:
    super().__init__()
    self.build_result = editor_control_wrapper.CommandResult(
        command=["bazel", "build", "//x"],
        returncode=0,
        durationMs=10,
        stdoutTail="",
        stderrTail="",
    )

  def run_build(self, timeout_ms):
    return self.build_result

  def restart_child(self, rebuild, timeout_ms=editor_control_wrapper.DEFAULT_BUILD_TIMEOUT_MS):
    self.restarted = True
    return (
        True,
        {
            "restarted": True,
            "build": None,
            "state": self.state(),
        },
    )


class EditorControlWrapperTests(unittest.TestCase):
  def test_message_round_trip(self) -> None:
    message = {
        "jsonrpc": "2.0",
        "id": 7,
        "method": "ping",
    }
    stream = io.BytesIO()

    editor_control_wrapper.write_message(stream, message)
    stream.seek(0)

    self.assertEqual(editor_control_wrapper.read_message(stream), message)

  def test_append_wrapper_tools_is_idempotent(self) -> None:
    response = {
        "jsonrpc": "2.0",
        "id": 1,
        "result": {
            "tools": [
                {
                    "name": "load_document",
                    "description": "child tool",
                }
            ]
        },
    }

    editor_control_wrapper.append_wrapper_tools(response)
    editor_control_wrapper.append_wrapper_tools(response)

    names = [tool["name"] for tool in response["result"]["tools"]]
    self.assertEqual(names.count(editor_control_wrapper.TOOL_STATE), 1)
    self.assertEqual(names.count(editor_control_wrapper.TOOL_RESTART), 1)
    self.assertEqual(names.count(editor_control_wrapper.TOOL_REBUILD), 1)
    self.assertIn("load_document", names)

  def test_state_tool_uses_local_proxy(self) -> None:
    result = editor_control_wrapper.handle_wrapper_tool(
        FakeProxy(),
        editor_control_wrapper.TOOL_STATE,
        {},
    )

    self.assertFalse(result.get("isError", False))
    self.assertIn("child_running", result["content"][0]["text"])

  def test_restart_tool_uses_local_proxy(self) -> None:
    fake_proxy = FakeProxy()
    result = editor_control_wrapper.handle_wrapper_tool(
        fake_proxy,
        editor_control_wrapper.TOOL_RESTART,
        {
            "rebuild": True,
            "timeout_ms": 1234,
        },
    )

    self.assertTrue(fake_proxy.restarted)
    self.assertFalse(result.get("isError", False))
    self.assertIn('"rebuild": true', result["content"][0]["text"])

  def test_rebuild_tool_keeps_build_result_after_restart(self) -> None:
    fake_proxy = FakeRebuildProxy()
    result = editor_control_wrapper.handle_wrapper_tool(
        fake_proxy,
        editor_control_wrapper.TOOL_REBUILD,
        {
            "restart": True,
        },
    )

    self.assertTrue(fake_proxy.restarted)
    self.assertFalse(result.get("isError", False))
    self.assertIn('"build": {', result["content"][0]["text"])
    self.assertIn('"ok": true', result["content"][0]["text"])

  def test_bazel_fallback_command_adds_output_root(self) -> None:
    command = ["bazel", "build", "//tools/mcp-servers/editor-control:editor_control_mcp_server"]

    fallback = editor_control_wrapper.bazel_command_with_output_root(command, "/tmp/bazel-root")

    self.assertEqual(
        fallback,
        [
            "bazel",
            "--output_user_root=/tmp/bazel-root",
            "build",
            "//tools/mcp-servers/editor-control:editor_control_mcp_server",
        ],
    )

  def test_output_base_permission_failure_detection(self) -> None:
    result = editor_control_wrapper.CommandResult(
        command=["bazel", "build", "//x"],
        returncode=36,
        durationMs=1,
        stdoutTail="",
        stderrTail="FATAL: Output base directory '/var/tmp/_bazel' must be readable and writable.",
    )

    self.assertTrue(editor_control_wrapper.output_base_permission_failure(result))


if __name__ == "__main__":
  unittest.main()
