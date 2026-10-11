#!/usr/bin/env bash
# Passes when the Geode embed example built and is in this test's runfiles as an executable.
set -euo pipefail
test -x "${TEST_SRCDIR:-.}/_main/examples/geode_embed" || test -x examples/geode_embed
