#!/usr/bin/env python3
"""Integration coverage for editor-control MCP GL framebuffer readback."""

from __future__ import annotations

import json
import os
from pathlib import Path
import select
import subprocess
import time
import unittest


def runfile(path: str) -> Path:
  """Resolve a Bazel runfile path for local and sandboxed test execution."""
  candidates: list[Path] = []
  workspace = os.environ.get("TEST_WORKSPACE", "_main")
  for env_name in ("RUNFILES_DIR", "TEST_SRCDIR"):
    env_value = os.environ.get(env_name)
    if not env_value:
      continue
    root = Path(env_value)
    candidates.append(root / path)
    candidates.append(root / workspace / path)

  cwd = Path.cwd()
  candidates.append(cwd / path)
  candidates.append(cwd / workspace / path)

  for candidate in candidates:
    if candidate.exists():
      return candidate

  raise FileNotFoundError(path)


class McpClient:
  def __init__(self, server: Path) -> None:
    # The configured MCP wrapper launches the raw bazel-bin server, not a
    # Bazel test/run runfiles process. Strip runfiles env vars so this test
    # covers that launch mode; otherwise macOS Cocoa initialization can pass
    # only in the Bazel-managed environment while the real MCP hangs.
    env = dict(os.environ)
    for name in ("RUNFILES_DIR", "RUNFILES_MANIFEST_FILE", "TEST_SRCDIR"):
      env.pop(name, None)
    env["DONNER_EDITOR_CONTROL_GL_READBACK_RUNNER"] = "bazel"
    self.proc = subprocess.Popen(
        [str(server)],
        cwd=Path(os.environ.get("BUILD_WORKING_DIRECTORY", Path.cwd())),
        env=env,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    self.next_id = 0
    self.stdout_buffer = b""
    self.stderr_buffer = b""
    self.stdout_open = True
    self.stderr_open = True

  def close(self) -> None:
    self.proc.terminate()
    try:
      self.proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
      self.proc.kill()
      self.proc.wait(timeout=2)

  def call(self, method: str, params: dict, timeout_seconds: float = 10.0) -> dict:
    self.next_id += 1
    message = {
        "jsonrpc": "2.0",
        "id": self.next_id,
        "method": method,
        "params": params,
    }
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    assert self.proc.stdin is not None
    self.proc.stdin.write(b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n\r\n")
    self.proc.stdin.write(body)
    self.proc.stdin.flush()
    return self.read_response(timeout_seconds)

  def read_response(self, timeout_seconds: float) -> dict:
    assert self.proc.stdout is not None
    assert self.proc.stderr is not None
    deadline = time.monotonic() + timeout_seconds
    while b"\r\n\r\n" not in self.stdout_buffer:
      self.read_available(deadline)
      if not self.stdout_open and b"\r\n\r\n" not in self.stdout_buffer:
        raise RuntimeError(self.stderr_tail())

    header, self.stdout_buffer = self.stdout_buffer.split(b"\r\n\r\n", 1)
    content_length = None
    for line in header.decode("utf-8", errors="replace").split("\r\n"):
      if line.lower().startswith("content-length:"):
        content_length = int(line.split(":", 1)[1].strip())
        break
    if content_length is None:
      raise RuntimeError(f"missing Content-Length header: {header!r}")

    while len(self.stdout_buffer) < content_length:
      self.read_available(deadline)
      if not self.stdout_open and len(self.stdout_buffer) < content_length:
        raise RuntimeError(self.stderr_tail())

    body = self.stdout_buffer[:content_length]
    self.stdout_buffer = self.stdout_buffer[content_length:]
    return json.loads(body)

  def read_available(self, deadline: float) -> None:
    streams = []
    if self.stdout_open:
      assert self.proc.stdout is not None
      streams.append(self.proc.stdout)
    if self.stderr_open:
      assert self.proc.stderr is not None
      streams.append(self.proc.stderr)
    if not streams:
      raise RuntimeError(self.stderr_tail())

    remaining = deadline - time.monotonic()
    if remaining <= 0:
      raise TimeoutError(self.stderr_tail())
    ready, _, _ = select.select(streams, [], [], min(remaining, 1.0))
    if not ready:
      return

    for stream in ready:
      chunk = os.read(stream.fileno(), 4096)
      if stream is self.proc.stdout:
        if chunk:
          self.stdout_buffer += chunk
        else:
          self.stdout_open = False
      else:
        if chunk:
          self.stderr_buffer += chunk
        else:
          self.stderr_open = False

  def stderr_tail(self) -> str:
    return self.stderr_buffer.decode("utf-8", errors="replace")[-4096:]


class EditorControlGlReadbackTests(unittest.TestCase):
  def test_replay_rnr_gl_readback_returns_imgui_framebuffer_png(self) -> None:
    server = runfile("tools/mcp-servers/editor-control/editor_control_mcp_server")
    rnr = runfile("zoom-out-drag-jump.rnr")
    svg = runfile("donner_splash_v0_8.svg")

    client = McpClient(server)
    try:
      client.call(
          "initialize",
          {
              "protocolVersion": "2024-11-05",
              "capabilities": {},
              "clientInfo": {"name": "editor-control-gl-readback-test", "version": "0"},
          },
          timeout_seconds=5.0,
      )

      response = client.call(
          "tools/call",
          {
              "name": "replay_rnr",
              "arguments": {
                  "rnr_path": str(rnr),
                  "svg_path": str(svg),
                  "render_each_frame": False,
                  "gl_readback": True,
                  "gl_capture_frame": 1,
                  "gl_max_frame": 1,
                  "gl_crop": "full",
                  "gl_output_dir": os.environ.get("TEST_TMPDIR", "/tmp"),
                  "gl_visible": True,
                  "gl_pace": False,
                  "gl_timeout_ms": 120000,
                  "include_gl_images": True,
                  "include_frame_results": False,
                  "max_frame_results": 0,
              },
          },
          timeout_seconds=150.0,
      )
    finally:
      client.close()

    result = response["result"]
    self.assertNotIn("isError", result, result)
    images = [item for item in result["content"] if item.get("type") == "image"]
    self.assertEqual(len(images), 1, result)
    self.assertEqual(images[0]["mimeType"], "image/png")

    text = "\n".join(item["text"] for item in result["content"] if item.get("type") == "text")
    payload = json.loads(text)
    self.assertTrue(payload["ok"], payload)
    self.assertEqual(payload["mode"], "gl_readback")
    self.assertEqual(payload["gl_readback_runner"], "bazel_run")
    self.assertEqual(payload["crop"], "full")
    self.assertGreater(payload["capture_count"], 0)


if __name__ == "__main__":
  unittest.main()
