#!/usr/bin/env bash
# Supplies commit provenance only to the app packaging action.
set -euo pipefail
revision="$(git rev-parse --verify HEAD)"
printf 'STABLE_DONNER_EDITOR_SOURCE_REVISION %s\n' "$revision"
