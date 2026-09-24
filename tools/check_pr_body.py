"""Reject tool-session links in pull request bodies."""

import json
import os
from pathlib import Path
import re
import sys


_TOOL_SESSION_LINK = re.compile(
    r"https?://(?:www\.)?(?:claude\.ai/code/session_[a-z0-9_-]+|"
    r"(?:chatgpt\.com|chat\.openai\.com)/(?:codex/tasks|c|share)/[a-z0-9_-]+)",
    re.IGNORECASE,
)


def has_tool_session_link(body: str) -> bool:
    """Return whether a PR body links to an agent or chat session."""
    return _TOOL_SESSION_LINK.search(body) is not None


def _pull_request_body(event_path: Path) -> str:
    event = json.loads(event_path.read_text(encoding="utf-8"))
    if not isinstance(event, dict) or not isinstance(event.get("pull_request"), dict):
        raise ValueError("missing pull request payload")
    body = event["pull_request"].get("body")
    if body is None:
        return ""
    if not isinstance(body, str):
        raise ValueError("pull request body is not text")
    return body


def main(argv: list[str] | None = None) -> int:
    """Check the event payload without echoing any URL into CI logs."""
    args = sys.argv[1:] if argv is None else argv
    if len(args) > 1:
        print("usage: check_pr_body.py [event.json]", file=sys.stderr)
        return 2
    event_path = args[0] if args else os.environ.get("GITHUB_EVENT_PATH")
    if not event_path:
        print("PR body check has no event payload", file=sys.stderr)
        return 2
    try:
        body = _pull_request_body(Path(event_path))
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError):
        print("PR body check could not read a pull request payload", file=sys.stderr)
        return 2
    if has_tool_session_link(body):
        print("PR body contains a tool-session link; remove it before merge", file=sys.stderr)
        return 1
    print("PR body link check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
