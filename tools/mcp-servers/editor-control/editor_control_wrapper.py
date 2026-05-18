#!/usr/bin/env python3
"""MCP stdio proxy for rebuilding and restarting the Donner editor control server."""

from __future__ import annotations

import dataclasses
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import threading
import time
from typing import Any, BinaryIO


JsonObject = dict[str, Any]

BUILD_TARGET = "//tools/mcp-servers/editor-control:editor_control_mcp_server"
DEFAULT_BUILD_TIMEOUT_MS = 10 * 60 * 1000
DEFAULT_FALLBACK_OUTPUT_USER_ROOT = "/private/tmp/donner-editor-control-bazel"
TOOL_REBUILD = "rebuild_editor_control_server"
TOOL_RESTART = "restart_editor_control_server"
TOOL_STATE = "editor_control_wrapper_state"


class ProtocolError(RuntimeError):
  """Raised when a stdio MCP message is malformed."""


@dataclasses.dataclass
class CommandResult:
  """Result from a wrapper-owned command."""

  command: list[str]
  returncode: int
  durationMs: int
  stdoutTail: str
  stderrTail: str

  def to_json(self) -> JsonObject:
    return {
        "command": self.command,
        "returncode": self.returncode,
        "duration_ms": self.durationMs,
        "stdout_tail": self.stdoutTail,
        "stderr_tail": self.stderrTail,
        "ok": self.returncode == 0,
    }


def command_result_from_completed(
    command: list[str],
    start: float,
    completed: subprocess.CompletedProcess[bytes],
) -> CommandResult:
  return CommandResult(
      command=command,
      returncode=completed.returncode,
      durationMs=int((time.monotonic() - start) * 1000),
      stdoutTail=tail_text(completed.stdout),
      stderrTail=tail_text(completed.stderr),
  )


def repo_root_from_script() -> Path:
  return Path(__file__).resolve().parents[3]


def bool_from_env(name: str, default: bool) -> bool:
  value = os.environ.get(name)
  if value is None:
    return default
  return value.strip().lower() in ("1", "true", "yes", "on")


def tail_text(value: bytes, limit: int = 16000) -> str:
  if len(value) > limit:
    value = value[-limit:]
  return value.decode("utf-8", errors="replace")


def output_base_permission_failure(result: CommandResult) -> bool:
  return (
      "Output base directory" in result.stderrTail
      and "must be readable and writable" in result.stderrTail
  )


def bazel_command_with_output_root(command: list[str], output_user_root: str) -> list[str] | None:
  if not command:
    return None
  executable = Path(command[0]).name
  if executable != "bazel" and executable != "llm-bazel-wrap.sh":
    return None
  return [command[0], f"--output_user_root={output_user_root}", *command[1:]]


def read_message(stream: BinaryIO) -> JsonObject | None:
  headers: dict[str, str] = {}

  while True:
    line = stream.readline()
    if line == b"":
      return None
    if line in (b"\r\n", b"\n"):
      break

    key, separator, value = line.decode("ascii", errors="replace").partition(":")
    if separator:
      headers[key.strip().lower()] = value.strip()

  content_length = headers.get("content-length")
  if content_length is None:
    raise ProtocolError("missing Content-Length header")

  try:
    length = int(content_length)
  except ValueError as exc:
    raise ProtocolError(f"invalid Content-Length header: {content_length}") from exc

  body = stream.read(length)
  if len(body) != length:
    return None
  return json.loads(body)


def write_message(stream: BinaryIO, message: JsonObject) -> None:
  body = json.dumps(message, separators=(",", ":")).encode("utf-8")
  stream.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
  stream.write(body)
  stream.flush()


def error_response(message_id: Any, code: int, message: str) -> JsonObject:
  return {
      "jsonrpc": "2.0",
      "id": message_id,
      "error": {
          "code": code,
          "message": message,
      },
  }


def tool_result(body: JsonObject, is_error: bool = False) -> JsonObject:
  result: JsonObject = {
      "content": [
          {
              "type": "text",
              "text": json.dumps(body, indent=2, sort_keys=True),
          }
      ]
  }
  if is_error:
    result["isError"] = True
  return result


def initialize_response(message_id: Any) -> JsonObject:
  return {
      "jsonrpc": "2.0",
      "id": message_id,
      "result": {
          "protocolVersion": "2024-11-05",
          "capabilities": {"tools": {}},
          "serverInfo": {
              "name": "donner-editor-control-wrapper",
              "version": "0.1.0",
          },
          "instructions": (
              "Wrapper for the Donner editor control MCP server. It can rebuild "
              "and restart the local C++ server, then proxy its editor-control tools."
          ),
      },
  }


def wrapper_tools() -> list[JsonObject]:
  return [
      {
          "name": TOOL_STATE,
          "description": "Inspect the editor-control wrapper, child process, and last build.",
          "inputSchema": {
              "type": "object",
              "properties": {},
              "additionalProperties": False,
          },
      },
      {
          "name": TOOL_RESTART,
          "description": "Restart the child C++ editor-control MCP server.",
          "inputSchema": {
              "type": "object",
              "properties": {
                  "rebuild": {
                      "type": "boolean",
                      "description": "Run bazel build before restarting.",
                      "default": False,
                  },
                  "timeout_ms": {
                      "type": "integer",
                      "description": "Maximum build time when rebuild is true.",
                      "default": DEFAULT_BUILD_TIMEOUT_MS,
                  },
              },
              "additionalProperties": False,
          },
      },
      {
          "name": TOOL_REBUILD,
          "description": "Rebuild the child C++ editor-control MCP server and optionally restart it.",
          "inputSchema": {
              "type": "object",
              "properties": {
                  "restart": {
                      "type": "boolean",
                      "description": "Restart the child server after a successful build.",
                      "default": True,
                  },
                  "timeout_ms": {
                      "type": "integer",
                      "description": "Maximum build time.",
                      "default": DEFAULT_BUILD_TIMEOUT_MS,
                  },
              },
              "additionalProperties": False,
          },
      },
  ]


def append_wrapper_tools(response: JsonObject) -> JsonObject:
  tools = response.setdefault("result", {}).setdefault("tools", [])
  existing_names = {tool.get("name") for tool in tools if isinstance(tool, dict)}
  for tool in wrapper_tools():
    if tool["name"] not in existing_names:
      tools.append(tool)
  return response


def is_wrapper_tool(name: str) -> bool:
  return name in {TOOL_STATE, TOOL_RESTART, TOOL_REBUILD}


class EditorControlProxy:
  """Owns the child C++ MCP server process."""

  def __init__(self) -> None:
    self.repoRoot = Path(os.environ.get("DONNER_EDITOR_CONTROL_REPO_ROOT", repo_root_from_script()))
    self.buildTarget = os.environ.get("DONNER_EDITOR_CONTROL_BUILD_TARGET", BUILD_TARGET)
    self.binaryPath = Path(
        os.environ.get(
            "DONNER_EDITOR_CONTROL_BINARY",
            self.repoRoot
            / "bazel-bin/tools/mcp-servers/editor-control/editor_control_mcp_server",
        )
    )
    child_command = os.environ.get("DONNER_EDITOR_CONTROL_CHILD_COMMAND")
    self.childCommand = shlex.split(child_command) if child_command else [str(self.binaryPath)]
    build_command = os.environ.get("DONNER_EDITOR_CONTROL_BUILD_COMMAND")
    self.buildCommand = (
        shlex.split(build_command)
        if build_command
        else ["bazel", "build", self.buildTarget]
    )
    self.fallbackOutputUserRoot = os.environ.get(
        "DONNER_EDITOR_CONTROL_BAZEL_OUTPUT_USER_ROOT",
        DEFAULT_FALLBACK_OUTPUT_USER_ROOT,
    )
    self.buildOnStart = bool_from_env("DONNER_EDITOR_CONTROL_BUILD_ON_START", False)
    self.initialBuildAttempted = False
    self.child: subprocess.Popen[bytes] | None = None
    self.lastBuild: CommandResult | None = None
    self.lastStartError: str | None = None

  def state(self) -> JsonObject:
    child_running = self.child is not None and self.child.poll() is None
    return {
        "repo_root": str(self.repoRoot),
        "binary_path": str(self.binaryPath),
        "build_target": self.buildTarget,
        "build_command": self.buildCommand,
        "fallback_output_user_root": self.fallbackOutputUserRoot,
        "child_command": self.childCommand,
        "child_running": child_running,
        "child_pid": self.child.pid if child_running and self.child is not None else None,
        "build_on_start": self.buildOnStart,
        "initial_build_attempted": self.initialBuildAttempted,
        "last_build": self.lastBuild.to_json() if self.lastBuild is not None else None,
        "last_start_error": self.lastStartError,
    }

  def run_build(self, timeout_ms: int = DEFAULT_BUILD_TIMEOUT_MS) -> CommandResult:
    result = self.run_command(self.buildCommand, timeout_ms)
    if result.returncode == 0 or not output_base_permission_failure(result):
      self.lastBuild = result
      return result

    fallback_command = bazel_command_with_output_root(
        self.buildCommand,
        self.fallbackOutputUserRoot,
    )
    if fallback_command is None:
      self.lastBuild = result
      return result

    fallback = self.run_command(fallback_command, timeout_ms)
    fallback.stderrTail = (
        "Primary build could not use the default Bazel output base; retried with "
        f"--output_user_root={self.fallbackOutputUserRoot}.\n\n"
        "Primary stderr:\n"
        f"{result.stderrTail}\n\n"
        "Retry stderr:\n"
        f"{fallback.stderrTail}"
    )
    fallback.stderrTail = fallback.stderrTail[-16000:]
    self.lastBuild = fallback
    return fallback

  def run_command(self, command: list[str], timeout_ms: int) -> CommandResult:
    start = time.monotonic()
    try:
      completed = subprocess.run(
          command,
          cwd=self.repoRoot,
          stdout=subprocess.PIPE,
          stderr=subprocess.PIPE,
          timeout=max(timeout_ms / 1000.0, 1.0),
          check=False,
      )
      return command_result_from_completed(command, start, completed)
    except subprocess.TimeoutExpired as exc:
      return CommandResult(
          command=command,
          returncode=124,
          durationMs=int((time.monotonic() - start) * 1000),
          stdoutTail=tail_text(exc.stdout or b""),
          stderrTail=tail_text(exc.stderr or b"") + "\nbuild timed out",
      )

  def ensure_child(self) -> tuple[bool, str | None]:
    if self.child is not None and self.child.poll() is None:
      return (True, None)

    if self.buildOnStart and not self.initialBuildAttempted:
      self.initialBuildAttempted = True
      build_result = self.run_build()
      if build_result.returncode != 0 and not self.binaryPath.exists():
        return (False, "initial build failed and no previous binary exists")

    return self.start_child()

  def start_child(self) -> tuple[bool, str | None]:
    self.lastStartError = None
    try:
      self.child = subprocess.Popen(
          self.childCommand,
          cwd=self.repoRoot,
          stdin=subprocess.PIPE,
          stdout=subprocess.PIPE,
          stderr=subprocess.PIPE,
      )
    except OSError as exc:
      self.child = None
      self.lastStartError = str(exc)
      return (False, self.lastStartError)

    if self.child.stderr is not None:
      thread = threading.Thread(
          target=self.copy_child_stderr,
          args=(self.child.stderr,),
          daemon=True,
      )
      thread.start()

    return (True, None)

  def copy_child_stderr(self, stderr: BinaryIO) -> None:
    while True:
      line = stderr.readline()
      if not line:
        return
      sys.stderr.buffer.write(b"[editor-control-child] " + line)
      sys.stderr.buffer.flush()

  def stop_child(self) -> None:
    if self.child is None:
      return
    child = self.child
    self.child = None
    if child.poll() is not None:
      return
    child.terminate()
    try:
      child.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
      child.kill()
      child.wait(timeout=2.0)

  def restart_child(
      self,
      rebuild: bool,
      timeout_ms: int = DEFAULT_BUILD_TIMEOUT_MS,
  ) -> tuple[bool, JsonObject]:
    build_json = None
    if rebuild:
      build_result = self.run_build(timeout_ms)
      build_json = build_result.to_json()
      if build_result.returncode != 0:
        return (
            False,
            {
                "restarted": False,
                "build": build_json,
                "state": self.state(),
            },
        )

    self.stop_child()
    started, start_error = self.start_child()
    return (
        started,
        {
            "restarted": started,
            "start_error": start_error,
            "build": build_json,
            "state": self.state(),
        },
    )

  def forward_to_child(self, request: JsonObject) -> JsonObject | None:
    started, start_error = self.ensure_child()
    if not started:
      return error_response(
          request.get("id"),
          -32000,
          f"editor-control child server is not running: {start_error}",
      )

    if self.child is None or self.child.stdin is None or self.child.stdout is None:
      return error_response(request.get("id"), -32000, "editor-control child pipes are unavailable")

    try:
      write_message(self.child.stdin, request)
      if "id" not in request:
        return None
      response = read_message(self.child.stdout)
    except (BrokenPipeError, OSError, ProtocolError, json.JSONDecodeError) as exc:
      self.stop_child()
      return error_response(request.get("id"), -32000, f"child server protocol failure: {exc}")

    if response is None:
      self.stop_child()
      return error_response(request.get("id"), -32000, "child server exited without a response")
    return response


def request_method(request: JsonObject) -> str:
  method = request.get("method")
  return method if isinstance(method, str) else ""


def tool_call_name(request: JsonObject) -> str | None:
  params = request.get("params")
  if not isinstance(params, dict):
    return None
  name = params.get("name")
  return name if isinstance(name, str) else None


def tool_call_arguments(request: JsonObject) -> JsonObject:
  params = request.get("params")
  if not isinstance(params, dict):
    return {}
  arguments = params.get("arguments")
  return arguments if isinstance(arguments, dict) else {}


def handle_wrapper_tool(proxy: EditorControlProxy, name: str, arguments: JsonObject) -> JsonObject:
  if name == TOOL_STATE:
    return tool_result(proxy.state())

  if name == TOOL_RESTART:
    rebuild = bool(arguments.get("rebuild", False))
    timeout_ms = int(arguments.get("timeout_ms", DEFAULT_BUILD_TIMEOUT_MS))
    ok, body = proxy.restart_child(rebuild=rebuild, timeout_ms=timeout_ms)
    return tool_result(body, is_error=not ok)

  if name == TOOL_REBUILD:
    restart = bool(arguments.get("restart", True))
    timeout_ms = int(arguments.get("timeout_ms", DEFAULT_BUILD_TIMEOUT_MS))
    build_result = proxy.run_build(timeout_ms)
    ok = build_result.returncode == 0
    body: JsonObject = {"build": build_result.to_json(), "restarted": False}
    if ok and restart:
      restarted, restart_body = proxy.restart_child(rebuild=False)
      if restart_body.get("build") is None:
        restart_body.pop("build", None)
      body.update(restart_body)
      ok = restarted
    else:
      body["state"] = proxy.state()
    return tool_result(body, is_error=not ok)

  return tool_result({"error": f"unknown wrapper tool: {name}"}, is_error=True)


def handle_request(proxy: EditorControlProxy, request: JsonObject) -> JsonObject | None:
  message_id = request.get("id")
  method = request_method(request)

  if method == "tools/call":
    name = tool_call_name(request)
    if name is None:
      return error_response(message_id, -32602, "tools/call params.name must be a string")
    if is_wrapper_tool(name):
      return {
          "jsonrpc": "2.0",
          "id": message_id,
          "result": handle_wrapper_tool(proxy, name, tool_call_arguments(request)),
      }

  if method == "tools/list":
    response = proxy.forward_to_child(request)
    if response is None or "error" in response:
      response = {"jsonrpc": "2.0", "id": message_id, "result": {"tools": []}}
    return append_wrapper_tools(response)

  response = proxy.forward_to_child(request)
  if response is None:
    return None
  if method == "initialize" and "error" in response:
    return initialize_response(message_id)
  return response


def main() -> int:
  proxy = EditorControlProxy()
  try:
    while True:
      try:
        request = read_message(sys.stdin.buffer)
      except (ProtocolError, json.JSONDecodeError) as exc:
        write_message(sys.stdout.buffer, error_response(None, -32700, str(exc)))
        continue

      if request is None:
        break
      response = handle_request(proxy, request)
      if response is not None:
        write_message(sys.stdout.buffer, response)
  finally:
    proxy.stop_child()
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
