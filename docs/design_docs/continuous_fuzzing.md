# Design: Continuous Fuzzing Harness

**Status:** Design
**Author:** Claude Opus 4.6
**Created:** 2026-04-06

## Summary

A continuous fuzzing harness for Donner's libFuzzer targets that runs fuzzers for extended
periods on a dedicated VM, stops when coverage plateaus, manages corpus growth over time, and
reports crashes via GitHub Issues. Runs are triggered on a schedule that ensures at most one run
every 2 hours but doesn't miss pushes to main for long. Fuzzer targets are auto-discovered via
Bazel query so new fuzzers are picked up automatically — no manifest to maintain.

## Goals

- Run all fuzzer targets for sustained periods, not just the 10-second smoke tests.
- Automatically stop each fuzzer when it stops finding new coverage (plateau detection).
- Maintain a persistent, deduplicated corpus that grows across runs and is checked into the repo.
- Detect new crashes, deduplicate them, and file GitHub Issues with reproduction artifacts.
- Trigger on pushes to main with rate limiting: at most once every 2 hours, but never more than
  2 hours stale if a push happened.
- Run on the local dedicated VM (not GitHub Actions runners) since fuzzing needs sustained compute.

## Non-Goals

- Distributed/cluster fuzzing (single VM is sufficient for now).
- Integrating with OSS-Fuzz or ClusterFuzz infrastructure.
- Fuzzing with engines other than libFuzzer (e.g., AFL, Honggfuzz).
- Automatic crash-fix generation — just reporting.
- Coverage-guided corpus selection across fuzzers (each fuzzer manages its own corpus).

## Next Steps

1. Review and iterate on this design doc until approved.
2. Implement Phase 1 (local scaffold) so fuzzing can be kicked off manually right away.
3. Once Phase 1 is validated, layer on automation (Phase 2).

## Implementation Plan

The plan is split into two phases: a **local-first scaffold** you can run immediately, then
**automation and reporting** layered on top.

### Phase 1: Local scaffold (run it now)

- [x] Milestone 1: Minimal runner you can invoke manually
  - [x] Create `tools/fuzzing/run_continuous_fuzz.py` with CLI interface
  - [x] Auto-discover fuzzer targets via `bazel query` (tag `fuzz_target`, kind `cc_binary`)
  - [x] Build all discovered targets with `--config=asan-fuzzer`
  - [x] Run fuzzers sequentially (simplest first) with configurable time per fuzzer
  - [x] Parse libFuzzer stderr stats and print a summary at the end
  - [x] Collect crash artifacts into an output directory
- [x] Milestone 2: Parallel execution and plateau detection
  - [x] Add `--workers=N` for parallel fuzzer execution
  - [x] Implement coverage plateau detection (stop fuzzer when `cov:` stalls for N minutes)
  - [x] Add `--max-total-time` and `--max-fuzzer-time` safeguards
  - [x] Add `--dry-run` mode for testing the orchestration without running fuzzers
- [x] Milestone 3: Corpus management
  - [x] Create `tools/fuzzing/manage_corpus.py` — merges, minimizes, and deduplicates corpus
  - [x] Pre-run: merge in-tree corpus + persistent corpus into working dir
  - [x] Post-run: minimize via `libFuzzer -merge=1` back into persistent corpus
  - [x] `manage_corpus.py update-intree` command to copy minimized corpus back to repo
  - [ ] Document corpus management workflow

### Phase 2: Automation and reporting

- [x] Milestone 4: Crash detection and issue filing
  - [x] Parse libFuzzer crash output to extract crash files, stack traces, and signal info
  - [x] Deduplicate crashes by stack trace signature (top N frames)
  - [x] File GitHub Issues via `gh` CLI with crash details, repro input (attached), and labels
  - [x] Optionally send notifications (email, Slack webhook, or similar)
- [x] Milestone 5: Trigger mechanism
  - [x] Create `tools/fuzzing/trigger_fuzz.sh` with rate-limiting and commit-tracking logic
  - [x] Create systemd timer + service unit (fires every 30 min, enforces 2-hour minimum
        interval)
  - [x] Implement "catch-up" logic: if main has new commits since last run and > 2 hours have
        passed, trigger immediately
  - [x] Add git fetch/polling to detect new pushes to main
- [x] Milestone 6: Observability, docs, and containerization
  - [x] Write run summaries to a log directory with per-fuzzer stats (coverage, corpus size,
        executions/sec, crashes found)
  - [x] Add dashboard script with health check, coverage trends, corpus history
  - [x] Update `docs/fuzzing.md` with continuous fuzzing documentation
  - [x] Dockerfile + docker-compose.yml for containerized fuzzing

## Background

Donner has a growing set of libFuzzer-based fuzz targets (currently 16, with more on the way)
covering parsers across base, CSS, SVG, and resource modules. Today these run only as:

1. **Corpus regression tests** — replay existing corpus files (no new input generation)
2. **10-second smoke tests** — brief fuzzing runs in CI

This is good for regression but leaves significant coverage on the table. Extended fuzzing runs
(hours to days) are where libFuzzer finds the interesting edge cases. The project has a dedicated
VM with ample compute, making it the right place for sustained fuzzing.

### Fuzzer target discovery

Targets are **auto-discovered** at runtime via Bazel query rather than maintained in a static
list. The `donner_cc_fuzzer` rule in `build_defs/rules.bzl` tags all fuzzer binaries with
`fuzz_target`, so the runner discovers them with:

```sh
bazel query 'attr(tags, "fuzz_target", //...) intersect kind("cc_binary", //...)'
```

This means adding a new fuzzer only requires defining it with `donner_cc_fuzzer()` — no runner
configuration changes needed. The corpus directory for each target is derived from the Bazel
`data` attribute (the `_corpus` filegroup) so that mapping is also automatic.

As of 2026-04-06, there are 16 targets across base, CSS, SVG, and resource modules, but this
number is expected to grow as new parsers and code paths are added.

### Prior art

- **OSS-Fuzz**: Google's continuous fuzzing service. Relevant design patterns: corpus management,
  crash deduplication by stack signature, coverage-guided stopping.

## Proposed Architecture

```
┌──────────────────────────────────────────────────────────┐
│                    Trigger Layer                          │
│                                                          │
│  systemd timer (every 30min)                             │
│       │                                                  │
│       ▼                                                  │
│  trigger_fuzz.sh                                         │
│    - git fetch origin main                               │
│    - check last_run_timestamp vs 2hr rate limit          │
│    - check if new commits since last run                 │
│    - if eligible: invoke run_continuous_fuzz.py           │
│                                                          │
└──────────────────┬───────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────┐
│              Runner (run_continuous_fuzz.py)              │
│                                                          │
│  1. git pull origin main                                 │
│  2. bazel build --config=asan-fuzzer <all fuzzer _bin>   │
│  3. For each fuzzer (parallel, up to N workers):         │
│     a. Create working corpus dir (merge in-tree + saved) │
│     b. Launch fuzzer binary with corpus dir              │
│     c. Monitor libFuzzer stats output (stderr)           │
│     d. Detect coverage plateau → stop fuzzer             │
│     e. On crash → save artifact, continue other fuzzers  │
│  4. Post-run: merge & minimize corpus                    │
│  5. Post-run: process crashes                            │
│  6. Write run summary                                    │
│                                                          │
└──────────┬──────────────────┬────────────────────────────┘
           │                  │
           ▼                  ▼
┌─────────────────┐  ┌────────────────────────────────────┐
│ Corpus Manager  │  │       Crash Reporter               │
│                 │  │                                     │
│ - Merge new     │  │ - Parse crash files + stack trace   │
│   inputs into   │  │ - Compute stack signature           │
│   persistent    │  │ - Deduplicate against known crashes │
│   corpus dir    │  │ - File GitHub Issue via `gh`        │
│ - Minimize      │  │ - Attach repro input                │
│   (libFuzzer    │  │ - Label: "fuzzing", "crash"         │
│   -merge=1)     │  │ - Optional: webhook notification    │
│ - Update        │  │                                     │
│   in-tree       │  └────────────────────────────────────┘
│   corpus        │
│                 │
└─────────────────┘
```

### Component Details

#### 1. Trigger Layer (`tools/fuzzing/trigger_fuzz.sh`)

A lightweight shell script invoked by a systemd timer every 30 minutes. It implements the
rate-limiting and freshness logic:

```
LAST_RUN_FILE=~/.donner-fuzz/last_run_timestamp
MIN_INTERVAL=7200  # 2 hours in seconds

now=$(date +%s)
last_run=$(cat "$LAST_RUN_FILE" 2>/dev/null || echo 0)
elapsed=$((now - last_run))

# Fetch latest main
git -C /home/jwm/Projects/donner fetch origin main

# Check if new commits exist since last run
LAST_RUN_COMMIT=$(cat ~/.donner-fuzz/last_run_commit 2>/dev/null || echo "")
CURRENT_HEAD=$(git -C /home/jwm/Projects/donner rev-parse origin/main)

if [ "$CURRENT_HEAD" = "$LAST_RUN_COMMIT" ]; then
    exit 0  # No new commits, nothing to do
fi

if [ "$elapsed" -lt "$MIN_INTERVAL" ]; then
    exit 0  # Rate limited, will catch it on next timer tick
fi

# Eligible — run fuzzing
echo "$now" > "$LAST_RUN_FILE"
echo "$CURRENT_HEAD" > ~/.donner-fuzz/last_run_commit
exec python3 tools/fuzzing/run_continuous_fuzz.py --commit="$CURRENT_HEAD"
```

The 30-minute timer tick combined with the 2-hour minimum interval means: after a push to main,
fuzzing starts within 30 minutes (on the next tick) but won't re-trigger for another 2 hours.
This satisfies "immediately if one hasn't happened in a while" without hammering the machine.

**systemd units:**

- `donner-fuzz.timer` — `OnCalendar=*:00/30` (every 30 minutes)
- `donner-fuzz.service` — runs `trigger_fuzz.sh`, `Type=oneshot`

#### 2. Runner (`tools/fuzzing/run_continuous_fuzz.py`)

The main orchestrator. Key design decisions:

**Target discovery:** Auto-discovered via Bazel query (see "Fuzzer target discovery" above).
No hardcoded target list — new fuzzers added with `donner_cc_fuzzer()` are picked up
automatically on the next run.

**Parallel execution:** Run up to `--workers=N` fuzzers concurrently (default: 4, tunable based
on VM cores). Each fuzzer gets its own subprocess. Use Python `subprocess.Popen` with stderr
piped for stats monitoring.

**Per-fuzzer libFuzzer flags:**
```
-max_total_time=3600     # 1 hour default (configurable)
-timeout=30              # Per-input timeout
-jobs=1                  # Single job per process (parallelism at runner level)
-print_final_stats=1     # Emit stats at end
-rss_limit_mb=4096       # Memory limit
```

**Coverage plateau detection:** libFuzzer periodically prints stats lines to stderr:
```
#12345  REDUCE cov: 1234 ft: 5678 corp: 100/50kb ...
```

The runner parses `cov:` (edge coverage) from these lines. If coverage hasn't increased in a
configurable window (default: 10 minutes of wall time with no new coverage), the fuzzer is
stopped early. This is the "stop when we stop gaining coverage" behavior.

Algorithm:
```python
last_cov_increase_time = time.monotonic()
last_cov = 0
PLATEAU_TIMEOUT = 600  # 10 minutes

for line in fuzzer_stderr:
    cov = parse_coverage(line)
    if cov > last_cov:
        last_cov = cov
        last_cov_increase_time = time.monotonic()
    elif time.monotonic() - last_cov_increase_time > PLATEAU_TIMEOUT:
        fuzzer_process.terminate()
        break
```

**Resource safeguards:**
- `--max-total-time=14400` (4 hours) hard cap on entire run
- `--max-fuzzer-time=3600` (1 hour) per fuzzer hard cap (overridden by plateau detection)
- RSS limit via libFuzzer's `-rss_limit_mb`

#### 3. Corpus Manager (`tools/fuzzing/manage_corpus.py`)

Manages the lifecycle of fuzz corpus data across runs.

**Corpus directory structure:**
```
~/.donner-fuzz/corpus/              # Persistent corpus (survives across runs)
  number_parser_fuzzer/
  decompress_fuzzer/
  ...
~/.donner-fuzz/workdir/             # Per-run working directory (ephemeral)
  number_parser_fuzzer/
  ...
```

**Pre-run merge:** Before each fuzzer runs, merge the in-tree corpus (e.g.,
`donner/css/parser/tests/color_parser_corpus/`) with the persistent corpus into the working
directory. This ensures the fuzzer starts from the best known inputs.

**Post-run merge + minimize:** After each fuzzer completes, use libFuzzer's built-in corpus
minimization:
```sh
./fuzzer_bin -merge=1 ~/.donner-fuzz/corpus/target/ workdir/target/ in_tree_corpus/
```
This deduplicates inputs and keeps only those that contribute unique coverage, preventing
unbounded corpus growth.

**In-tree corpus update:** A separate command (`manage_corpus.py update-intree`) copies the
minimized persistent corpus back to the in-tree corpus directories. This is intentionally a
manual step (or can be automated with a PR) so that corpus updates go through code review:

```sh
# After a fuzzing session, update the in-tree corpus and submit a PR
python3 tools/fuzzing/manage_corpus.py update-intree
git checkout -b fuzzing/corpus-update-$(date +%Y%m%d)
git add donner/*/tests/*_corpus/ donner/*/*/tests/*_corpus/
git commit -m "fuzzing: update corpus from continuous fuzzing run"
gh pr create --title "fuzzing: corpus update" --body "Automated corpus update from continuous fuzzing"
```

**Corpus statistics:** Track corpus size (file count and bytes) per target over time. Log to
`~/.donner-fuzz/stats/corpus_history.jsonl` for trend monitoring.

#### 4. Crash Reporter

**Crash detection:** libFuzzer writes crash-reproducing inputs to files named
`crash-<sha1>`, `timeout-<sha1>`, or `oom-<sha1>` in the working directory. The runner collects
these after each fuzzer completes.

**Stack signature:** Run the fuzzer binary on the crash input to reproduce and capture the stack
trace. Compute a signature from the top 5 unique frames (excluding common frames like
`LLVMFuzzerTestOneInput`, `__asan_*`, `libc`). This signature is used for deduplication.

**Deduplication:** Maintain a ledger at `~/.donner-fuzz/known_crashes.json` mapping stack
signatures to GitHub Issue numbers. Skip filing if the signature is already known.

**GitHub Issue filing:**
```sh
gh issue create \
  --title "Fuzzing crash: ${FUZZER_NAME} — ${CRASH_TYPE} in ${TOP_FRAME}" \
  --label "fuzzing,crash,automated" \
  --body "$(cat <<EOF
## Fuzzing Crash Report

**Fuzzer:** \`${FUZZER_TARGET}\`
**Crash type:** ${CRASH_TYPE} (${SIGNAL})
**Commit:** ${COMMIT_SHA}
**Date:** $(date -u +%Y-%m-%dT%H:%M:%SZ)

### Stack Trace
\`\`\`
${STACK_TRACE}
\`\`\`

### Reproduction
\`\`\`sh
bazel build --config=asan-fuzzer ${BAZEL_TARGET}
${BINARY_PATH} crash_input_file
\`\`\`

The crash input file is attached below.

### Fuzzer Stats
- Executions: ${TOTAL_EXECS}
- Coverage at crash: ${COV} edges
- Corpus size: ${CORPUS_SIZE} inputs
EOF
)"
```

Crash input files are attached to the issue or stored in a known location referenced by the
issue.

**Notifications:** Beyond GitHub Issues, support an optional webhook URL (configured in
`~/.donner-fuzz/config.json`) for Slack/email/etc. notifications. The webhook receives a JSON
payload with crash summary.

## Data and State

All persistent state lives under `~/.donner-fuzz/`:

```
~/.donner-fuzz/
  config.json              # Configuration (webhook URL, parallelism, timeouts)
  last_run_timestamp       # Unix timestamp of last run start
  last_run_commit          # Git SHA of last fuzzing run
  corpus/                  # Persistent merged corpus (per-target subdirs)
  known_crashes.json       # Stack signature → Issue number mapping
  stats/
    corpus_history.jsonl   # Corpus size trends
    run_history.jsonl      # Per-run summaries
  logs/
    YYYYMMDD-HHMMSS/       # Per-run log directory
      runner.log           # Orchestrator log
      <target>.log         # Per-fuzzer stderr output
      <target>.stats       # Per-fuzzer final stats
      crashes/             # Crash artifacts from this run
```

## Security / Privacy

- Fuzz inputs are untrusted by definition — the entire point is to feed hostile data to parsers.
  The ASAN + UBSAN instrumentation (`--config=asan-fuzzer`) catches memory safety issues.
- Crash inputs may contain arbitrary binary data. They're stored locally and attached to
  GitHub Issues in a private or public repo (depending on repo visibility). No sensitive data
  is involved since inputs are randomly generated.
- The runner executes fuzzer binaries as the local user. Resource limits (RSS, timeout) prevent
  runaway processes.
- The `gh` CLI uses the locally configured GitHub auth token. No additional credentials are
  stored by this system.

## Testing and Validation

- **Unit tests for Python scripts:** Test coverage plateau detection, stats parsing, crash
  deduplication, and corpus merge logic. Use pytest.
- **Integration test:** A small end-to-end test that builds one fuzzer, runs for 10 seconds,
  verifies corpus output and stats logging. Can run in CI.
- **Manual validation:** Run the full harness on the VM and verify:
  - Fuzzers start and stop based on plateau detection
  - Corpus is merged and minimized correctly
  - Crash reporting files a real GitHub Issue
  - Rate limiting works (trigger twice within 2 hours, second is skipped)
- **Dry-run mode:** `--dry-run` flag that does everything except actually run fuzzers and file
  issues, for testing the orchestration logic.

## Alternatives Considered

**OSS-Fuzz integration:** Would provide Google's infrastructure but requires conforming to their
build system and submitting the project. Overkill for a single-VM setup and adds external
dependency. Can be considered later as the project grows.

**GitHub Actions for fuzzing:** GitHub runners have time limits (6 hours max) and limited
compute. Not suitable for sustained multi-hour fuzzing. The dedicated VM is a better fit.

**AFL++ instead of libFuzzer:** The existing fuzzers all use the libFuzzer API
(`LLVMFuzzerTestOneInput`). Switching would require build system changes. libFuzzer's
`-merge=1` and stats output are well-suited to the corpus management and plateau detection needs.

**Coverage-guided stopping via llvm-cov:** More accurate than parsing libFuzzer stats, but
significantly more complex (requires profdata merging, report generation). The libFuzzer
`cov:` counter is a good-enough proxy for plateau detection.

## Open Questions

1. **Corpus PR automation:** Should corpus updates be fully automated (bot creates PR) or
   semi-manual (script prepares branch, human creates PR)? Leaning toward automated PR creation
   with a human merge gate.
2. **Crash severity classification:** Should we attempt to classify crashes by severity
   (e.g., null deref vs. heap-buffer-overflow) and label issues accordingly?
3. **Fuzzer priority/weighting:** Should some fuzzers get more time than others (e.g., complex
   parser fuzzers vs. simple leaf parsers)? Or just give all fuzzers equal time and let plateau
   detection handle it naturally? As the fuzzer count grows this becomes more relevant.
4. **Notification channel:** What's the preferred notification mechanism beyond GitHub Issues?
   Slack webhook, email, or just Issues are sufficient?

## Future Work

- [ ] Add coverage tracking and trend reports (total coverage over time across all fuzzers)
- [ ] Integrate with a coverage dashboard (e.g., Codecov fuzzing-specific profile)
- [ ] Support dictionary files for structured fuzzers (XML, CSS, SVG)
- [ ] Auto-generate corpus seed inputs from the test suite's SVG/CSS fixtures
- [ ] Investigate structure-aware fuzzing (custom mutators for SVG/CSS grammar)
