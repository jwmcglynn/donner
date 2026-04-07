# Donner Continuous Fuzzing

Long-lived Docker container that continuously fuzzes all Donner parser
targets, files GitHub Issues for crashes, and maintains a growing corpus.

No repo clone needed on the host — Docker pulls everything from GitHub.

## Setup

### Prerequisites

- Docker Engine 24+ with Compose V2
- At least 8 CPU cores and 16 GB RAM available for the container
- (Optional) A GitHub personal access token for crash issue filing

### 1. Download the compose file

```sh
mkdir donner-fuzz && cd donner-fuzz
curl -O https://raw.githubusercontent.com/jwmcglynn/donner/main/tools/fuzzing/docker-compose.yml
```

### 2. (Optional) Set up crash reporting

To have crashes automatically filed as GitHub Issues, create a token at
https://github.com/settings/tokens with `repo` scope, then uncomment
the `GH_TOKEN` line in `docker-compose.yml`:

```yaml
environment:
  - GH_TOKEN=ghp_your_token_here
```

### 3. Start

```sh
docker compose up -d
```

That's it. Docker will:

1. Build the fuzzing image (pulls Dockerfile from GitHub, installs
   Bazel + clang + gh CLI)
2. Start a long-lived container that loops every 30 minutes:
   - Clones the donner repo on first start (incremental fetches after)
   - Checks for new commits on `main`
   - Builds all fuzzer targets with ASAN + libFuzzer
   - Runs all fuzzers in parallel until coverage plateaus
   - Minimizes and merges the corpus
   - Reports any new crashes via GitHub Issues
3. If the host is under load (CPU steal > 10%), automatically reduces
   to 2 workers

The first run takes longer (~5-10 min) to clone and build from scratch.
Subsequent runs use the Bazel cache and only rebuild what changed.

### 4. Verify it's running

```sh
docker compose logs -f fuzz
```

You should see the entrypoint cloning the repo, then the trigger loop
starting.

## Day-to-day usage

### Watch logs

```sh
docker compose logs -f fuzz
```

### Dashboard (coverage trends, run history, crash summary)

```sh
docker compose exec fuzz python3 tools/fuzzing/dashboard.py
```

### Trigger a run immediately

```sh
docker compose exec fuzz tools/fuzzing/trigger_fuzz.sh --force
```

### One-shot run (for CI or manual)

```sh
docker compose run --rm fuzz-once
```

### View known crashes

```sh
docker compose exec fuzz python3 tools/fuzzing/crash_reporter.py list
```

### Corpus stats

```sh
docker compose exec fuzz python3 tools/fuzzing/manage_corpus.py stats
```

### Stop

```sh
docker compose down
```

Persistent data (corpus, crash history, Bazel cache) is stored in Docker
volumes and survives `down` / `up` cycles. To wipe everything and start
fresh:

```sh
docker compose down -v
```

## Configuration

Edit environment variables in `docker-compose.yml`, or override at
runtime:

```sh
FUZZ_WORKERS=4 FUZZ_MAX_TOTAL=7200 docker compose up -d
```

| Variable | Default | Description |
|---|---|---|
| `FUZZ_REPO_URL` | `https://github.com/jwmcglynn/donner.git` | Repo to clone and fuzz |
| `FUZZ_MIN_INTERVAL` | 7200 (2h) | Minimum seconds between runs |
| `FUZZ_LOOP_INTERVAL` | 1800 (30m) | How often to check for new commits |
| `FUZZ_WORKERS` | 8 | Parallel fuzzer count |
| `FUZZ_FUZZER_TIME` | 900 (15m) | Max time per fuzzer |
| `FUZZ_PLATEAU` | 120 (2m) | Stop a fuzzer after this long with no new coverage |
| `FUZZ_MAX_TOTAL` | 3600 (1h) | Max total wall time per run |
| `FUZZ_QUIET_MODE` | `reduce` | `reduce`, `skip`, or `ignore` when host is busy |
| `FUZZ_QUIET_WORKERS` | 2 | Workers in quiet mode |
| `FUZZ_STEAL_THRESHOLD` | 10 | CPU steal % that triggers quiet mode |
| `GH_TOKEN` | (unset) | GitHub token for crash issue filing |

## Persistent volumes

| Volume | Contents |
|---|---|
| `donner-fuzz-state` | Corpus, crash ledger, run reports, trigger logs |
| `donner-fuzz-bazel-cache` | Bazel build cache (incremental builds) |
| `donner-fuzz-repo` | Git checkout (incremental fetches) |

## Architecture

```
┌─────────────────────────────────────────────────┐
│  Docker container (donner-fuzz)                  │
│                                                  │
│  entrypoint.sh (loop every 30 min)               │
│    └─ trigger_fuzz.sh                            │
│         ├─ git fetch origin main                 │
│         ├─ check: new commits? rate limit?       │
│         ├─ check: CPU steal (quiet hours)        │
│         ├─ run_continuous_fuzz.py --minimize      │
│         │    ├─ bazel build --config=asan-fuzzer │
│         │    ├─ run fuzzers in parallel          │
│         │    ├─ plateau detection → stop early   │
│         │    └─ corpus minimize (post-run)        │
│         └─ crash_reporter.py report --latest     │
│              ├─ reproduce crashes                │
│              ├─ deduplicate by stack signature   │
│              └─ gh issue create (if GH_TOKEN)    │
│                                                  │
│  Volumes:                                        │
│    /home/fuzzer/.donner-fuzz  (state)            │
│    /home/fuzzer/.cache/bazel  (build cache)      │
│    /home/fuzzer/donner        (repo checkout)    │
└─────────────────────────────────────────────────┘
```
