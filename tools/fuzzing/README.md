# Donner Continuous Fuzzing

Long-lived Docker container that continuously fuzzes all Donner parser
targets, files GitHub Issues for crashes, and maintains a growing corpus.

## Setup (Docker)

### Prerequisites

- Docker Engine 24+ with Compose V2
- At least 8 CPU cores and 16 GB RAM available for the container
- (Optional) A GitHub personal access token for crash issue filing

### 1. Clone the repo on your host

```sh
git clone https://github.com/jwmcglynn/donner.git
cd donner
```

### 2. (Optional) Configure GitHub token for crash reporting

Create a token at https://github.com/settings/tokens with `repo` scope,
then uncomment and set `GH_TOKEN` in `tools/fuzzing/docker-compose.yml`:

```yaml
environment:
  - GH_TOKEN=ghp_your_token_here
```

Or pass it at runtime:

```sh
GH_TOKEN=ghp_your_token_here docker compose -f tools/fuzzing/docker-compose.yml up -d
```

### 3. Build and start

```sh
cd tools/fuzzing
docker compose build
docker compose up -d
```

That's it. The container will:

1. Clone the donner repo from GitHub (first start only)
2. Check for new commits on `main` every 30 minutes
3. When new commits arrive (and the 2-hour rate limit has elapsed):
   - Pull the latest `main`
   - Build all fuzzer targets with ASAN + libFuzzer
   - Run all fuzzers in parallel until coverage plateaus
   - Minimize and merge the corpus
   - Report any crashes via GitHub Issues (if `GH_TOKEN` is set)
4. If the host is under load (CPU steal > 10%), automatically
   reduce to 2 workers

The first run takes longer (~5-10 min) to build from scratch. Subsequent
runs use the Bazel cache and only rebuild what changed.

### 4. Verify it's running

```sh
docker compose -f tools/fuzzing/docker-compose.yml logs -f fuzz
```

You should see the entrypoint cloning the repo, then the trigger loop
starting.

## Day-to-day usage

All commands assume you're in the repo root or `tools/fuzzing/`.

### Watch logs

```sh
docker compose -f tools/fuzzing/docker-compose.yml logs -f fuzz
```

### Dashboard

```sh
docker compose -f tools/fuzzing/docker-compose.yml exec fuzz \
  python3 tools/fuzzing/dashboard.py
```

### Trigger a run immediately

```sh
docker compose -f tools/fuzzing/docker-compose.yml exec fuzz \
  tools/fuzzing/trigger_fuzz.sh --force
```

### One-shot run (CI or manual)

```sh
docker compose -f tools/fuzzing/docker-compose.yml run --rm fuzz-once
```

### View known crashes

```sh
docker compose -f tools/fuzzing/docker-compose.yml exec fuzz \
  python3 tools/fuzzing/crash_reporter.py list
```

### Corpus stats

```sh
docker compose -f tools/fuzzing/docker-compose.yml exec fuzz \
  python3 tools/fuzzing/manage_corpus.py stats
```

### Stop

```sh
docker compose -f tools/fuzzing/docker-compose.yml down
```

Persistent data (corpus, crash history, Bazel cache) is stored in Docker
volumes and survives `down` / `up` cycles. To wipe everything:

```sh
docker compose -f tools/fuzzing/docker-compose.yml down -v
```

## Configuration

Edit `docker-compose.yml` environment variables, or override at runtime:

```sh
FUZZ_WORKERS=4 FUZZ_MAX_TOTAL=7200 docker compose up -d
```

| Variable | Default | Description |
|---|---|---|
| `FUZZ_REPO_URL` | `https://github.com/jwmcglynn/donner.git` | Repo to clone and fuzz |
| `FUZZ_MIN_INTERVAL` | 7200 (2h) | Minimum seconds between runs |
| `FUZZ_LOOP_INTERVAL` | 1800 (30m) | How often the trigger checks for new commits |
| `FUZZ_WORKERS` | 8 | Parallel fuzzer count |
| `FUZZ_FUZZER_TIME` | 900 (15m) | Max time per fuzzer |
| `FUZZ_PLATEAU` | 120 (2m) | Stop a fuzzer after this long with no new coverage |
| `FUZZ_MAX_TOTAL` | 3600 (1h) | Max total wall time per run |
| `FUZZ_QUIET_MODE` | `reduce` | `reduce`, `skip`, or `ignore` when host is busy |
| `FUZZ_QUIET_WORKERS` | 2 | Workers in quiet mode |
| `FUZZ_STEAL_THRESHOLD` | 10 | CPU steal % that triggers quiet mode |
| `GH_TOKEN` | (unset) | GitHub token for crash issue filing |

## Persistent volumes

| Volume | Path in container | Contents |
|---|---|---|
| `donner-fuzz-state` | `~/.donner-fuzz` | Corpus, crash ledger, run reports, logs |
| `donner-fuzz-bazel-cache` | `~/.cache/bazel` | Bazel build cache (incremental builds) |
| `donner-fuzz-repo` | `~/donner` | Git checkout (incremental fetches) |

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
