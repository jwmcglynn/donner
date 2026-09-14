# Fuzzing {#Fuzzing}

The parsers and subparsers in Donner SVG have fuzzers that harden the implementation and surface
new edge cases. Fuzzing uses [libFuzzer](https://llvm.org/docs/LibFuzzer.html).

## Running a Fuzzer

To run a fuzzer, first build it with `--config=asan-fuzzer`:

```sh
bazel build --config=asan-fuzzer //donner/css/parser:declaration_list_parser_fuzzer
```

Then run it, passing a directory to hold the corpus. It runs until it hits a crash or is stopped
with Ctrl-C.

```sh
mkdir ~/declcorpus
bazel-bin/donner/css/parser/declaration_list_parser_fuzzer ~/declcorpus/
```

To raise throughput, run several jobs at once:

```sh
bazel-bin/donner/css/parser/declaration_list_parser_fuzzer ~/declcorpus/ -jobs=8
```

When a failure occurs, libFuzzer writes a repro file. Copy it into the in-tree corpus directory to
guard against regressions; corpus files are checked during normal `bazel test //...` runs.

```sh
mv ./crash-0f6f12d023e0ad5ed83b29763cd67315e0ee0c6b donner/css/parser/tests/declaration_list_parser_corpus/
```

# To run all fuzz tests

```sh
bazel test --config=asan-fuzzer --test_tag_filters=fuzz_target //...
```

# Continuous Fuzzing {#ContinuousFuzzing}

Donner includes a continuous fuzzing harness that runs all fuzzer targets for extended periods,
stops when coverage plateaus, manages the corpus across runs, and reports crashes.

See the [design doc](design_docs/0012-continuous_fuzzing.md) for the architecture.

## Quick Start

```sh
# Run all fuzzers (5 min each, 4 workers):
python3 tools/fuzzing/run_continuous_fuzz.py

# Longer run with 8 workers, 15 min per fuzzer, 1 hour total cap:
python3 tools/fuzzing/run_continuous_fuzz.py --workers=8 --fuzzer-time=900 --max-total-time=3600

# Filter to specific fuzzers:
python3 tools/fuzzing/run_continuous_fuzz.py --filter=svg_parser

# Run + auto-minimize corpus afterward:
python3 tools/fuzzing/run_continuous_fuzz.py --minimize
```

## Plateau Detection

Fuzzers stop automatically when edge coverage stops growing. The `--plateau-timeout` flag
(default: 10 minutes) sets how long to wait after the last new coverage before terminating,
so a saturated fuzzer does not keep consuming compute.

## Corpus Management

```sh
# Minimize the latest run into persistent corpus:
python3 tools/fuzzing/manage_corpus.py minimize --latest

# Copy persistent corpus into in-tree directories (for committing):
python3 tools/fuzzing/manage_corpus.py update-intree

# Show corpus sizes:
python3 tools/fuzzing/manage_corpus.py stats
```

## Crash Reporting

```sh
# Process crashes from the latest run (files GitHub Issues):
python3 tools/fuzzing/crash_reporter.py report --latest

# Dry run (detect + dedup only, no issue filing):
python3 tools/fuzzing/crash_reporter.py report --latest --dry-run

# List known crashes:
python3 tools/fuzzing/crash_reporter.py list
```

## Dashboard

```sh
python3 tools/fuzzing/dashboard.py           # Summary of recent runs
python3 tools/fuzzing/dashboard.py --json    # Machine-readable output
```

## Automated Runs (Docker)

The harness runs in a Docker container with docker compose:

```sh
cd tools/fuzzing
docker compose up -d          # Start continuous fuzzing
docker compose logs -f fuzz   # Watch output
docker compose exec fuzz python3 tools/fuzzing/dashboard.py  # Dashboard
docker compose down           # Stop
```

See `tools/fuzzing/docker-compose.yml` for configuration.
