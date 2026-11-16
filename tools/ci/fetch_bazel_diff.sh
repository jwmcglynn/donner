#!/usr/bin/env bash
#
# fetch_bazel_diff.sh - download the pinned bazel-diff release JAR to $RUNNER_TEMP/bazel-diff.jar
# and refuse it unless it matches the pinned digest.
#
# Usage: tools/ci/fetch_bazel_diff.sh
#
# Reads the release tag and digest from BAZEL_DIFF_TAG and BAZEL_DIFF_SHA256. Transient
# release-CDN failures (dropped connections, 5xx) have failed CI runs outright, so the download is
# retried as a whole with backoff, and curl also retries inside each attempt.

set -euo pipefail

if [[ $# -ne 0 ]]; then
  echo "usage: tools/ci/fetch_bazel_diff.sh" >&2
  exit 2
fi
url="https://github.com/Tinder/bazel-diff/releases/download/${BAZEL_DIFF_TAG}/bazel-diff_deploy.jar"
out="$RUNNER_TEMP/bazel-diff.jar"
for delay in 2 4 8 16 0; do
  if curl -fsSL --retry 4 --retry-delay 2 --retry-all-errors \
      --connect-timeout 15 --max-time 300 -o "$out" "$url" \
      && [[ -s "$out" ]]; then
    if printf '%s  %s\n' "$BAZEL_DIFF_SHA256" "$out" \
        | sha256sum --check --status; then
      exit 0
    fi
    echo "bazel-diff digest mismatch for ${BAZEL_DIFF_TAG}; refusing artifact." >&2
  fi
  rm -f "$out"
  if [[ "$delay" -eq 0 ]]; then
    break
  fi
  echo "bazel-diff download failed; retrying in ${delay}s." >&2
  sleep "$delay"
done
echo "error: bazel-diff download failed after 5 attempts." >&2
exit 1
