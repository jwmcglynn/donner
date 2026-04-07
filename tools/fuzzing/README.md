# Donner Continuous Fuzzing

Long-lived Docker container that continuously fuzzes all Donner parser
targets, files GitHub Issues for crashes, and maintains a growing corpus.

No repo clone needed on the host — Docker pulls everything from GitHub.

## Setup

### Prerequisites

- Docker Engine 24+ with Compose V2
- At least 8 CPU cores and 16 GB RAM available for the container
- (Optional) A GitHub personal access token for crash issue filing

### 1. Download the compose file and create data directory

```sh
mkdir donner-fuzz && cd donner-fuzz
curl -O https://raw.githubusercontent.com/jwmcglynn/donner/main/tools/fuzzing/docker-compose.yml
mkdir -p fuzz-data/donner
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
docker compose logs -f donner
```

You should see the entrypoint cloning the repo, then the trigger loop
starting. The web dashboard is at **http://localhost:8080**.

## Day-to-day usage

### Web dashboard

Open **http://localhost:8080** — auto-refreshes every 30s. Shows:

- Status badge (fuzzing / idle), key metrics
- Recent runs table with execution counts
- Coverage trends across runs (with deltas)
- Corpus minimization history
- Known crashes with GitHub Issue links
- Latest trigger log

JSON API at `http://localhost:8080/api/status`.

Change the port: `FUZZ_DASHBOARD_PORT=9090 docker compose up -d`

### Watch logs

```sh
docker compose logs -f donner
```

### CLI dashboard (inside container)

```sh
docker compose exec donner python3 tools/fuzzing/dashboard.py
```

### Trigger a run immediately

```sh
docker compose exec donner tools/fuzzing/trigger_fuzz.sh --force
```

### One-shot run (for CI or manual)

```sh
docker compose run --rm donner-once
```

### View known crashes

```sh
docker compose exec donner python3 tools/fuzzing/crash_reporter.py list
```

### Corpus stats

```sh
docker compose exec donner python3 tools/fuzzing/manage_corpus.py stats
```

### Stop

```sh
docker compose down
```

Persistent data lives at `./fuzz-data/` on the host and survives
`down` / `up` cycles. To relocate:

```sh
FUZZ_HOST_DIR=/mnt/storage docker compose up -d
```

To wipe everything and start fresh:

```sh
docker compose down -v
rm -rf fuzz-data/
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

## Adding more repos

The compose file supports multiple repos. For each new repo:

1. Duplicate the `donner:` service block in `docker-compose.yml`
2. Change the service name, container name, `FUZZ_REPO_URL`, and volume names
3. Create the host data directory: `mkdir -p fuzz-data/<repo-name>`
4. Add the corresponding volume entries

Example — adding `my-other-project`:

```yaml
  my-other-project:
    <<: *fuzz-defaults
    container_name: fuzz-my-other-project
    environment:
      - FUZZ_REPO_URL=https://github.com/your-org/your-repo.git
      - FUZZ_WORKERS=4
      # ... (copy remaining env vars from donner block)
    volumes:
      - ${FUZZ_HOST_DIR:-./fuzz-data}/my-other-project:/home/fuzzer/.donner-fuzz
      - my-other-project-cache:/home/fuzzer/.cache/bazel
      - my-other-project-repo:/home/fuzzer/donner
```

Then add the volumes:

```yaml
volumes:
  my-other-project-cache:
    name: fuzz-my-other-project-cache
  my-other-project-repo:
    name: fuzz-my-other-project-repo
```

Each repo gets its own container, Bazel cache, and state directory.
The repos fuzz independently but share the host machine (quiet hours
applies to all).

## Persistent data

| Location | Contents |
|---|---|
| `./fuzz-data/<repo>/` | Corpus, crash ledger, run reports, trigger logs (host-bound) |
| `fuzz-<repo>-bazel-cache` | Bazel build cache — Docker volume (incremental builds) |
| `fuzz-<repo>-repo` | Git checkout — Docker volume (incremental fetches) |

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
