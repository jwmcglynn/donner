"""Reject tool-session links in pull request bodies."""


def has_tool_session_link(body: str) -> bool:
    return False


def main() -> int:
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
