#!/usr/bin/env bash
#
# Entrypoint for the continuous fuzzing container.
# Replaces systemd timer with a simple loop.
#
# Runs trigger_fuzz.sh on a configurable interval, pulling latest main
# each iteration. Supports one-shot mode for CI or manual runs.
#
# Environment variables:
#   FUZZ_LOOP_INTERVAL  Seconds between trigger checks (default: 1800 = 30 min)
#   FUZZ_ONE_SHOT       Set to "1" to run once and exit (for CI)
#   All FUZZ_* variables from trigger_fuzz.sh are also supported.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="${FUZZ_REPO_DIR:-/home/fuzzer/donner}"
LOOP_INTERVAL="${FUZZ_LOOP_INTERVAL:-1800}"
ONE_SHOT="${FUZZ_ONE_SHOT:-0}"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [entrypoint] $*"
}

FUZZ_REPO_URL="${FUZZ_REPO_URL:-https://github.com/jwmcglynn/donner.git}"

# Clone on first start, then fetch+reset on subsequent iterations
ensure_repo() {
    if [ ! -d "$REPO_DIR/.git" ]; then
        log "Cloning repo from $FUZZ_REPO_URL..."
        git clone --branch main "$FUZZ_REPO_URL" "$REPO_DIR"
    fi
}

update_repo() {
    log "Fetching latest main..."
    git -C "$REPO_DIR" fetch origin main --quiet 2>/dev/null || {
        log "WARNING: git fetch failed"
    }
    git -C "$REPO_DIR" checkout main --quiet 2>/dev/null || true
    git -C "$REPO_DIR" reset --hard origin/main --quiet 2>/dev/null || {
        log "WARNING: git reset failed, running on current state"
    }
}

run_trigger() {
    log "Running fuzzing trigger..."
    "$REPO_DIR/tools/fuzzing/trigger_fuzz.sh" "$@" || {
        local exit_code=$?
        log "Trigger exited with code $exit_code"
        return $exit_code
    }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

log "Donner continuous fuzzing container starting"
log "  Repo: $REPO_DIR"
log "  Loop interval: ${LOOP_INTERVAL}s"
log "  One-shot: $ONE_SHOT"

ensure_repo

if [ "$ONE_SHOT" = "1" ]; then
    update_repo
    run_trigger --force
    exit $?
fi

# Loop mode — long-lived container that replaces systemd timer.
# Runs indefinitely, checking for new commits every LOOP_INTERVAL.
while true; do
    update_repo
    run_trigger || true  # Don't exit the loop on trigger failure

    log "Sleeping ${LOOP_INTERVAL}s until next check..."
    sleep "$LOOP_INTERVAL"
done
