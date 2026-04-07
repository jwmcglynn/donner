#!/usr/bin/env bash
#
# Trigger script for continuous fuzzing of Donner.
#
# Designed to be called by a systemd timer (every 30 minutes) or cron.
# Checks if main has new commits since the last run and enforces a minimum
# interval between runs (default: 2 hours).
#
# Usage:
#   ./trigger_fuzz.sh                  # Normal operation
#   ./trigger_fuzz.sh --force          # Skip rate limit and commit checks
#   ./trigger_fuzz.sh --dry-run        # Check eligibility without running
#   FUZZ_MIN_INTERVAL=3600 ./trigger_fuzz.sh  # Override interval (1 hour)
#
# Environment variables:
#   FUZZ_MIN_INTERVAL   Minimum seconds between runs (default: 7200 = 2 hours)
#   FUZZ_REPO_DIR       Path to the donner repo (default: script's repo root)
#   FUZZ_STATE_DIR      State directory (default: ~/.donner-fuzz)
#   FUZZ_WORKERS        Number of parallel fuzzers (default: 8)
#   FUZZ_FUZZER_TIME    Max time per fuzzer in seconds (default: 900 = 15 min)
#   FUZZ_PLATEAU        Plateau timeout in seconds (default: 120 = 2 min)
#   FUZZ_MAX_TOTAL      Max total run time in seconds (default: 3600 = 1 hour)
#   FUZZ_LOG_DIR        Log directory (default: $FUZZ_STATE_DIR/trigger-logs)
#   FUZZ_QUIET_MODE     When host contention is detected: "reduce" (default),
#                       "skip" (don't fuzz at all), or "ignore" (full speed)
#   FUZZ_QUIET_WORKERS  Worker count when in quiet mode (default: 2)
#   FUZZ_QUIET_MAX_TOTAL  Max total time in quiet mode (default: 1800 = 30 min)
#   FUZZ_LOAD_THRESHOLD 1-min load average threshold (default: 0 = disabled)
#   FUZZ_STEAL_THRESHOLD  CPU steal time % above which quiet mode activates
#                       (default: 10). Steal time indicates the hypervisor is
#                       giving CPU to other VMs/host processes — the best
#                       signal that other users are active on the host.

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FUZZ_REPO_DIR="${FUZZ_REPO_DIR:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
FUZZ_STATE_DIR="${FUZZ_STATE_DIR:-$HOME/.donner-fuzz}"
FUZZ_MIN_INTERVAL="${FUZZ_MIN_INTERVAL:-7200}"
FUZZ_WORKERS="${FUZZ_WORKERS:-8}"
FUZZ_FUZZER_TIME="${FUZZ_FUZZER_TIME:-900}"
FUZZ_PLATEAU="${FUZZ_PLATEAU:-120}"
FUZZ_MAX_TOTAL="${FUZZ_MAX_TOTAL:-3600}"
FUZZ_LOG_DIR="${FUZZ_LOG_DIR:-$FUZZ_STATE_DIR/trigger-logs}"
FUZZ_QUIET_MODE="${FUZZ_QUIET_MODE:-reduce}"
FUZZ_QUIET_WORKERS="${FUZZ_QUIET_WORKERS:-2}"
FUZZ_QUIET_MAX_TOTAL="${FUZZ_QUIET_MAX_TOTAL:-1800}"
FUZZ_LOAD_THRESHOLD="${FUZZ_LOAD_THRESHOLD:-0}"
FUZZ_STEAL_THRESHOLD="${FUZZ_STEAL_THRESHOLD:-10}"

TIMESTAMP_FILE="$FUZZ_STATE_DIR/last_run_timestamp"
COMMIT_FILE="$FUZZ_STATE_DIR/last_run_commit"
LOCK_FILE="$FUZZ_STATE_DIR/trigger.lock"

FORCE=0
DRY_RUN=0

for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        --dry-run) DRY_RUN=1 ;;
        --help|-h)
            head -25 "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

die() {
    log "ERROR: $*" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# Lock (prevent concurrent runs)
# ---------------------------------------------------------------------------

acquire_lock() {
    mkdir -p "$FUZZ_STATE_DIR"
    if [ -f "$LOCK_FILE" ]; then
        local lock_pid
        lock_pid=$(cat "$LOCK_FILE" 2>/dev/null || echo "")
        if [ -n "$lock_pid" ] && kill -0 "$lock_pid" 2>/dev/null; then
            log "Another trigger is running (PID $lock_pid). Exiting."
            exit 0
        fi
        log "Stale lock file found (PID $lock_pid not running). Removing."
        rm -f "$LOCK_FILE"
    fi
    echo $$ > "$LOCK_FILE"
}

release_lock() {
    rm -f "$LOCK_FILE"
}

trap release_lock EXIT

# ---------------------------------------------------------------------------
# Eligibility checks
# ---------------------------------------------------------------------------

check_rate_limit() {
    if [ "$FORCE" -eq 1 ]; then
        log "Rate limit bypassed (--force)"
        return 0
    fi

    local now last_run elapsed
    now=$(date +%s)
    last_run=$(cat "$TIMESTAMP_FILE" 2>/dev/null || echo 0)
    elapsed=$((now - last_run))

    if [ "$elapsed" -lt "$FUZZ_MIN_INTERVAL" ]; then
        local remaining=$((FUZZ_MIN_INTERVAL - elapsed))
        log "Rate limited: ${elapsed}s since last run, need ${FUZZ_MIN_INTERVAL}s (${remaining}s remaining)"
        return 1
    fi

    log "Rate limit OK: ${elapsed}s since last run (minimum: ${FUZZ_MIN_INTERVAL}s)"
    return 0
}

check_new_commits() {
    if [ "$FORCE" -eq 1 ]; then
        log "Commit check bypassed (--force)"
        return 0
    fi

    # Fetch latest main
    log "Fetching origin/main..."
    git -C "$FUZZ_REPO_DIR" fetch origin main --quiet 2>/dev/null || {
        log "WARNING: git fetch failed, proceeding with local state"
    }

    local current_head last_commit
    current_head=$(git -C "$FUZZ_REPO_DIR" rev-parse origin/main 2>/dev/null || \
                   git -C "$FUZZ_REPO_DIR" rev-parse main)
    last_commit=$(cat "$COMMIT_FILE" 2>/dev/null || echo "")

    if [ "$current_head" = "$last_commit" ]; then
        log "No new commits on main (HEAD: ${current_head:0:12})"
        return 1
    fi

    if [ -z "$last_commit" ]; then
        log "First run — no previous commit recorded"
    else
        local count
        count=$(git -C "$FUZZ_REPO_DIR" rev-list --count "$last_commit".."$current_head" 2>/dev/null || echo "?")
        log "New commits on main: ${count} commits since ${last_commit:0:12} (HEAD: ${current_head:0:12})"
    fi
    return 0
}

# ---------------------------------------------------------------------------
# Quiet hours (shared VM detection)
# ---------------------------------------------------------------------------

get_other_users() {
    # List unique logged-in users excluding the current user.
    # Returns one username per line, or empty if no other users.
    who | awk '{print $1}' | sort -u | grep -v "^$(whoami)$" || true
}

get_load_average() {
    # Get the 1-minute load average as an integer (rounded up).
    awk '{printf "%d\n", $1 + 0.5}' /proc/loadavg 2>/dev/null || echo 0
}

get_cpu_steal_pct() {
    # Measure CPU steal time percentage over a 1-second sample.
    # Steal time is the % of CPU cycles taken by the hypervisor for other
    # VMs or host processes. High steal = the physical host is busy.
    # Returns an integer percentage (0-100).
    #
    # /proc/stat cpu line format:
    #   cpu user nice system idle iowait irq softirq steal guest guest_nice
    local line1 line2
    line1=$(head -1 /proc/stat 2>/dev/null) || { echo 0; return; }
    sleep 1
    line2=$(head -1 /proc/stat 2>/dev/null) || { echo 0; return; }

    # Parse the two samples and compute steal delta / total delta
    echo "$line1
$line2" | awk '
    NR==1 {
        for (i=2; i<=NF; i++) total1 += $i
        steal1 = $10  # steal is the 9th value after "cpu", field 10
    }
    NR==2 {
        for (i=2; i<=NF; i++) total2 += $i
        steal2 = $10
        dt = total2 - total1
        ds = steal2 - steal1
        if (dt > 0) printf "%d\n", (ds * 100) / dt
        else print 0
    }'
}

check_quiet_hours() {
    # Checks if the host is under contention (CPU steal, other users, load).
    # Sets QUIET=1 if quiet mode should be activated.
    # Returns 0 if fuzzing should proceed (possibly with reduced resources),
    # returns 1 if fuzzing should be skipped entirely.
    QUIET=0

    if [ "$FUZZ_QUIET_MODE" = "ignore" ]; then
        log "Quiet hours: disabled (FUZZ_QUIET_MODE=ignore)"
        return 0
    fi

    # 1. CPU steal time — best signal for host contention in a VM.
    #    When the hypervisor gives CPU to other VMs, steal time rises.
    #    Set to -1 to disable this check entirely.
    if [ "$FUZZ_STEAL_THRESHOLD" -ge 0 ]; then
        local steal
        steal=$(get_cpu_steal_pct)
        if [ "$steal" -ge "$FUZZ_STEAL_THRESHOLD" ]; then
            log "Quiet hours: CPU steal time (${steal}%) >= threshold (${FUZZ_STEAL_THRESHOLD}%)"
            QUIET=1
        else
            log "Quiet hours: CPU steal time ${steal}% (threshold: ${FUZZ_STEAL_THRESHOLD}%)"
        fi
    fi

    # 2. Check for other logged-in users (useful if users SSH into the VM).
    if [ "$QUIET" -eq 0 ]; then
        local other_users
        other_users=$(get_other_users)
        if [ -n "$other_users" ]; then
            local user_list
            user_list=$(echo "$other_users" | tr '\n' ', ' | sed 's/,$//')
            log "Quiet hours: other users detected: $user_list"
            QUIET=1
        fi
    fi

    # 3. Check system load if threshold is set.
    if [ "$FUZZ_LOAD_THRESHOLD" -gt 0 ] && [ "$QUIET" -eq 0 ]; then
        local load
        load=$(get_load_average)
        if [ "$load" -ge "$FUZZ_LOAD_THRESHOLD" ]; then
            log "Quiet hours: load average ($load) >= threshold ($FUZZ_LOAD_THRESHOLD)"
            QUIET=1
        fi
    fi

    if [ "$QUIET" -eq 0 ]; then
        log "Quiet hours: host is idle, running at full capacity"
        return 0
    fi

    # Quiet mode is active — decide what to do
    case "$FUZZ_QUIET_MODE" in
        skip)
            log "Quiet hours: skipping fuzzing (FUZZ_QUIET_MODE=skip)"
            return 1
            ;;
        reduce)
            log "Quiet hours: reducing to $FUZZ_QUIET_WORKERS workers, ${FUZZ_QUIET_MAX_TOTAL}s max"
            FUZZ_WORKERS="$FUZZ_QUIET_WORKERS"
            FUZZ_MAX_TOTAL="$FUZZ_QUIET_MAX_TOTAL"
            return 0
            ;;
        *)
            log "WARNING: unknown FUZZ_QUIET_MODE=$FUZZ_QUIET_MODE, treating as 'reduce'"
            FUZZ_WORKERS="$FUZZ_QUIET_WORKERS"
            FUZZ_MAX_TOTAL="$FUZZ_QUIET_MAX_TOTAL"
            return 0
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Run fuzzing
# ---------------------------------------------------------------------------

run_fuzzing() {
    local current_head
    current_head=$(git -C "$FUZZ_REPO_DIR" rev-parse origin/main 2>/dev/null || \
                   git -C "$FUZZ_REPO_DIR" rev-parse main)
    local now
    now=$(date +%s)

    # Update checkout to latest main
    log "Updating repo to ${current_head:0:12}..."
    git -C "$FUZZ_REPO_DIR" checkout main --quiet 2>/dev/null || true
    git -C "$FUZZ_REPO_DIR" pull --ff-only --quiet 2>/dev/null || {
        log "WARNING: git pull failed, running on current state"
    }

    # Set up logging
    mkdir -p "$FUZZ_LOG_DIR"
    local log_file="$FUZZ_LOG_DIR/$(date '+%Y%m%d-%H%M%S').log"

    log "Starting fuzzing run..."
    log "  Workers: $FUZZ_WORKERS"
    log "  Fuzzer time: ${FUZZ_FUZZER_TIME}s"
    log "  Plateau timeout: ${FUZZ_PLATEAU}s"
    log "  Max total time: ${FUZZ_MAX_TOTAL}s"
    log "  Log file: $log_file"

    # Run the fuzzer with minimize
    python3 "$SCRIPT_DIR/run_continuous_fuzz.py" \
        --workers="$FUZZ_WORKERS" \
        --fuzzer-time="$FUZZ_FUZZER_TIME" \
        --plateau-timeout="$FUZZ_PLATEAU" \
        --max-total-time="$FUZZ_MAX_TOTAL" \
        --minimize \
        2>&1 | tee "$log_file"

    local fuzz_exit=${PIPESTATUS[0]}

    # Process crashes
    log "Processing crashes..."
    python3 "$SCRIPT_DIR/crash_reporter.py" report --latest 2>&1 | tee -a "$log_file"

    # Record successful run
    echo "$now" > "$TIMESTAMP_FILE"
    echo "$current_head" > "$COMMIT_FILE"

    log "Run complete (exit code: $fuzz_exit)"
    log "  Commit: ${current_head:0:12}"
    log "  Log: $log_file"

    return $fuzz_exit
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

main() {
    log "Donner continuous fuzzing trigger"
    log "  Repo: $FUZZ_REPO_DIR"
    log "  State: $FUZZ_STATE_DIR"

    acquire_lock

    # Check eligibility
    local eligible=1

    if ! check_new_commits; then
        eligible=0
    fi

    if ! check_rate_limit; then
        eligible=0
    fi

    if [ "$eligible" -eq 0 ]; then
        log "Not eligible for fuzzing. Exiting."
        exit 0
    fi

    # Check if VM is shared — may reduce workers or skip entirely
    if ! check_quiet_hours; then
        exit 0
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        if [ "$QUIET" -eq 1 ]; then
            log "Dry run: would start fuzzing in quiet mode ($FUZZ_WORKERS workers). Exiting."
        else
            log "Dry run: would start fuzzing now. Exiting."
        fi
        exit 0
    fi

    run_fuzzing
}

main
