# Fuzzing {#Fuzzing}

Parsers and subparsers within Donner SVG have fuzzers in order to harden the implementation and detect new edge cases. Fuzzing is performed with [libFuzzer](https://llvm.org/docs/LibFuzzer.html).

## Running a Fuzzer

To run a fuzzer, first build it with `--config=asan-fuzzer`:

```sh
bazel build --config=asan-fuzzer //donner/css/parser:declaration_list_parser_fuzzer
```

Then run it and pass it a directory to use for building the corpus. This will run indefinitely, until either a crash has been encountered or it is terminated with a Ctrl-C.

```sh
mkdir ~/declcorpus
bazel-bin/donner/css/parser/declaration_list_parser_fuzzer ~/declcorpus/
```

To maximum throughput, run with multiple simultaneous jobs:

```sh
bazel-bin/donner/css/parser/declaration_list_parser_fuzzer ~/declcorpus/ -jobs=8
```

When a failure occurs, a repro file is saved out. To guard against future crashes, copy the file to the corpus directory in-tree. This will then be validated during normal `bazel test //...` runs.

```sh
mv ./crash-0f6f12d023e0ad5ed83b29763cd67315e0ee0c6b donner/css/parser/tests/declaration_list_parser_corpus/
```

# To run all fuzz tests

```sh
bazel test --config=asan-fuzzer --test_tag_filters=fuzz_target //...
```

# Continuous Fuzzing {#ContinuousFuzzing}

Donner includes a continuous fuzzing harness that runs all fuzzer targets for extended periods,
stops when coverage plateaus, manages corpus across runs, and reports crashes.

See the [design doc](design_docs/0012-continuous_fuzzing.md) for full architecture details.

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

Fuzzers automatically stop when edge coverage stops growing. The `--plateau-timeout` flag
(default: 10 minutes) sets how long to wait after the last new coverage before terminating.
This avoids wasting compute on saturated fuzzers.

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

The harness runs in a Docker container via docker-compose:

```sh
cd tools/fuzzing
docker compose up -d          # Start continuous fuzzing
docker compose logs -f fuzz   # Watch output
docker compose exec fuzz python3 tools/fuzzing/dashboard.py  # Dashboard
docker compose down           # Stop
```

See `tools/fuzzing/docker-compose.yml` for configuration.
