#!/usr/bin/env bash
set -euo pipefail

if "$1" > "$TEST_TMPDIR/link_input_audit.txt" 2>&1; then
  echo "Additional linker input escaped the configured dependency audit" >&2
  exit 1
fi
grep -F "Forbidden dependency:" "$TEST_TMPDIR/link_input_audit.txt"

if "$2" > "$TEST_TMPDIR/link_option_audit.txt" 2>&1; then
  echo "Linker option escaped the configured dependency audit" >&2
  exit 1
fi
grep -F "Forbidden linker option:" "$TEST_TMPDIR/link_option_audit.txt"

if "$3" > "$TEST_TMPDIR/heredoc_audit.txt" 2>&1; then
  echo "A linker option changed the generated audit script" >&2
  exit 1
fi
grep -F "Forbidden linker option:" "$TEST_TMPDIR/heredoc_audit.txt"
