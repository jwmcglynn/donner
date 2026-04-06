#!/bin/bash
# Verifies that no BUILD files depend directly on external libraries that must
# be accessed through perf-sensitive wrappers in //third_party.
#
# When a library like zlib is used both by the Skia rendering pipeline (forced
# to opt mode) and by other parts of the project, all usages must go through
# the //third_party wrapper so the dep is always in the same configuration.
#
# To fix a violation: replace "@zlib" with "//third_party:zlib" (etc.) in the
# flagged BUILD file.
set -euo pipefail

# Resolve the workspace root from runfiles.
if [[ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]]; then
    ROOT="$BUILD_WORKSPACE_DIRECTORY"
elif [[ -n "${TEST_SRCDIR:-}" ]]; then
    ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE:-_main}"
else
    ROOT="."
fi

# Banned patterns and the wrapper file that is allowed to reference them.
BANNED=('"@zlib"' '"@freetype"')
ALLOWED="third_party/BUILD.bazel"

status=0

for pattern in "${BANNED[@]}"; do
    # Search all BUILD files for the banned dep, excluding the wrapper.
    violations=$(grep -rn "$pattern" \
        --include="BUILD.bazel" --include="BUILD" \
        "$ROOT" \
        | grep -v "$ALLOWED" || true)

    if [ -n "$violations" ]; then
        dep_name="${pattern#'\"@'}"
        dep_name="${dep_name%'\"'}"
        echo "ERROR: Direct dependency on $pattern is banned."
        echo "       Use \"//third_party:${dep_name}\" instead."
        echo ""
        echo "$violations"
        echo ""
        status=1
    fi
done

if [ $status -eq 0 ]; then
    echo "OK: No banned direct dependencies found."
fi

exit $status
