"""Expand excluded test suites before invoking a Bazel test command."""

import argparse
import re
import subprocess
import sys


def excluded_tests(bazel, suite):
    """Return concrete negative patterns, preserving query errors and diagnostics."""
    result = subprocess.run(
        [*bazel, "query", "--output=label", f"tests({suite})"],
        stdout=subprocess.PIPE,
        text=True,
        check=True,
    )
    labels = result.stdout.splitlines()
    if not labels or any(not re.fullmatch(r"//[\w./+-]+:[\w./+-]+", label) for label in labels):
        raise ValueError(f"Exclusion suite {suite} returned no tests or invalid labels")
    return ["-" + label for label in sorted(set(labels))]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exclude-suite", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if not re.fullmatch(r"//tools/ci:[\w-]+", args.exclude_suite):
        parser.error("--exclude-suite must name a //tools/ci test suite")
    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    if "test" not in command or command.index("test") == 0:
        parser.error("expected a Bazel executable, optional startup flags, and test command")
    # A negative suite pattern does not remove its members from recursive selections.
    negatives = excluded_tests(command[:command.index("test")], args.exclude_suite)
    if "--" not in command:
        command.append("--")
    return subprocess.run([*command, *negatives], check=False).returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
    except (OSError, ValueError) as error:
        print(f"CI test selection failed: {error}", file=sys.stderr)
        sys.exit(1)
