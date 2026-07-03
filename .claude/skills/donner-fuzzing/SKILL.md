---
name: donner-fuzzing
description: >
  Run, add, and triage Donner's libFuzzer fuzzers: the donner_cc_fuzzer three-target pattern,
  --config=asan-fuzzer, the manual deep-fuzz loop, corpus lifecycle, and the continuous-fuzzing
  harness in tools/fuzzing/. Use when a parser change warrants fuzz validation, when reproducing
  or fixing a crash-* / leak-* / timeout-* artifact or a fuzzer-filed GitHub issue, when adding a
  fuzzer for a new parser, or when managing fuzz corpora (manage_corpus.py, *_corpus dirs).
---

# Donner Fuzzing

Donner fuzzes its parsers with libFuzzer (https://llvm.org/docs/LibFuzzer.html). Every fuzzer is
declared with the `donner_cc_fuzzer` macro (`build_defs/rules.bzl`, `def donner_cc_fuzzer`), and
each in-tree corpus doubles as a permanent regression suite that replays under `bazel test`.

For depth beyond this how-to: `docs/fuzzing.md` (corpus + CLI walkthrough),
`docs/design_docs/0012-continuous_fuzzing.md` (harness architecture), `tools/fuzzing/README.md`
(Docker deployment).

## 1. The three-target pattern

`donner_cc_fuzzer(name, srcs, corpus, deps)` emits THREE targets per fuzzer:

| Target              | Kind      | What it does                                                                                                                                                                                                                                                       |
| ------------------- | --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `{name}`            | cc_test   | Replays every file in the in-tree corpus once (libFuzzer regression mode). Runs in plain `bazel test //...` on Linux — but that build carries NO sanitizer (ASAN only comes from `--config=asan*`), so it only guards crashes that reproduce unsanitized (see §3). |
| `{name}_10_seconds` | cc_test   | Live-fuzzes for 10 s (`-max_total_time=10 -timeout=2`), size `large`. Smoke check that the harness still finds coverage.                                                                                                                                           |
| `{name}_bin`        | cc_binary | The binary for manual/long-running fuzzing. Not a test; you run it by hand with a corpus directory.                                                                                                                                                                |

All three link the libFuzzer runtime, so the plain `{name}` test binary in `bazel-bin/` also
accepts libFuzzer flags when invoked directly.

Inventory (do NOT trust a hardcoded list — fuzzers get added and deleted; this query is the
source of truth):

```sh
bazel query 'attr(tags, "fuzz_target", //...) intersect kind("cc_binary", //...)'
```

Corpus directories live next to the fuzzer, named `<fuzzer>_corpus` (e.g.
`donner/css/parser/tests/declaration_list_parser_corpus/`,
`donner/base/xml/tests/xml_tokenizer_corpus/`). Exception: `woff_parser_fuzzer` reuses
`donner/base/fonts/testdata/` as its corpus.

## 2. Running all fuzz tests

```sh
bazel test --config=asan-fuzzer \
    --test_tag_filters=fuzz_target --build_tag_filters=fuzz_target //...
```

`--build_tag_filters=fuzz_target` is NOT optional: `--test_tag_filters` only restricts which
tests _run_, so without it the command builds the entire repo under the asan-fuzzer toolchain
(~10k actions), and any unrelated build break reports every fuzz test as `NO STATUS`. With both
filters (exactly what `fuzz.yml` uses) only the fuzzers build.

Why `--config=asan-fuzzer` (defined in `.bazelrc`) is mandatory on macOS: Apple Clang ships no
`libclang_rt.fuzzer_osx.a`, so fuzz targets are marked `@platforms//:incompatible` off Linux
unless `//build_defs:fuzzers_enabled` is set (`fuzzer_compatible_with()` in
`build_defs/rules.bzl`). `asan-fuzzer` implies `--config=asan` → `--config=latest_llvm` →
`--//build_defs:llvm_latest=1`, which activates the hermetic LLVM 21 toolchain that carries the
fuzzer runtime.

**Failure mode this prevents:** plain `bazel test //...` on macOS SILENTLY skips every fuzz
target (they're "incompatible", not failed). A parser crash that a corpus file would catch then
escapes to Linux CI or the nightly fuzz workflow. If you touched a parser, run the tag-filtered
asan-fuzzer command above before pushing.

CI: `.github/workflows/fuzz.yml` runs the corpus regression suite daily at 06:00 UTC, on manual
dispatch, and on PRs touching `**/*_fuzzer.cc|cpp|h`, `**/corpus/**`, or `build_defs/rules.bzl`.
Two caveats when reading that workflow:

- The `**/corpus/**` path filter matches NO in-tree corpus dir — they are all named `*_corpus/`
  (or `testdata/`), and the glob needs a segment literally named `corpus`. A corpus-only PR
  (e.g. committing a crash file per §3) therefore does NOT trigger the workflow; only the daily
  run replays it. The glob likely wants `**/*_corpus/**` — an upstream fix candidate.
- Check the current gating status — as of this writing the test step carries `|| true`
  (informational, not blocking), so a green checkmark there does NOT prove fuzzers pass.

## 3. Manual deep-fuzz loop (after a parser change, or to reproduce a crash)

```sh
# 1. Build the fuzzer with sanitizers + libFuzzer. `{name}` and `{name}_bin` link the same
#    code + libFuzzer runtime, so either works for manual fuzzing (§1); `{name}` reuses the
#    binary `bazel test` already built:
bazel build --config=asan-fuzzer //donner/css/parser:declaration_list_parser_fuzzer

# 2. Run against a scratch corpus dir (runs until crash or Ctrl-C; -jobs for parallelism):
mkdir -p ~/declcorpus
bazel-bin/donner/css/parser/declaration_list_parser_fuzzer ~/declcorpus/ -jobs=8

# 3. To reproduce a specific crash artifact, pass the file as the only argument:
bazel-bin/donner/css/parser/declaration_list_parser_fuzzer ./crash-0f6f12d0...

# 4. Optionally shrink the input first (standard libFuzzer flag):
bazel-bin/donner/css/parser/declaration_list_parser_fuzzer -minimize_crash=1 -runs=10000 ./crash-...
```

On a crash, libFuzzer writes `crash-<sha1>` (or `leak-*` / `timeout-*` / `oom-*`) to the cwd.

**Crash-fix protocol** (this is the fuzzing form of donner-bugfix-discipline's red→green rule):

1. Copy the (minimized) crash file into the fuzzer's in-tree corpus directory, e.g.
   `mv crash-... donner/css/parser/tests/declaration_list_parser_corpus/`.
2. Commit the corpus file FIRST and confirm the `{name}` replay test fails at that commit —
   that is your red. The red check must run under ASAN:
   `bazel test --config=asan-fuzzer //donner/css/parser:declaration_list_parser_fuzzer`.
   A plain (unsanitized) run can stay green for memory-safety bugs, and on macOS a plain
   `bazel test` silently skips the target (§2) — either way a false-negative red check.
3. Fix the parser; the replay test goes green. The corpus file then re-executes the crashing
   bytes in every `bazel test //...` run on Linux (unsanitized) and in the daily asan-fuzzer
   workflow — which is currently non-blocking (§2). For sanitizer-only bugs there is thus no
   blocking gate yet; the committed input is still the strongest guard available.
4. After the replay test goes green, re-fuzz the fixed target live for adjacent crashes — a
   crash fix often sits next to sibling bugs in the same code path, and the corpus-replay test
   only re-executes the one known input. Run the §3 loop for at least several minutes
   (`bazel-bin/<path>/<fuzzer> ~/scratch-corpus -jobs=8`), or
   `python3 tools/fuzzing/run_continuous_fuzz.py --filter=<fuzzer>` for a longer soak.

A crash fix WITHOUT the crash input committed to the in-tree corpus is an unverified fix:
nothing re-executes the crashing bytes, so the bug can silently regress.

**Reproducing a fuzzer-filed GitHub issue:** the issue body carries the stack trace, commit, and
a repro command template — but NOT the crash bytes. `crash_reporter.py` never attaches the input
file (`file_github_issue` builds the body from the stack trace only; the `<crash_input_file>` in
the repro command is a placeholder). The bytes exist only on the host that found the crash: look
up the issue's signature in that host's `~/.donner-fuzz/known_crashes.json` and fetch the file at
its `crash_file` path (for the Docker deployment, from inside the container — see
`tools/fuzzing/README.md`). If the file is gone, re-find the crash by fuzzing that target (§4,
`--filter=`).

## 4. Continuous-fuzzing harness (`tools/fuzzing/`)

For long unattended runs, use the harness instead of hand-looping. It auto-discovers targets via
the bazel query above, builds them with `--config=asan-fuzzer`, and runs them in parallel with
plateau detection (a fuzzer stops after `--plateau-timeout` seconds with no new edge coverage).
State lives under `~/.donner-fuzz/` (`runs/<timestamp>/`, `corpus/`). For current worker/time
defaults, check `--help` or the `DEFAULT_*` constants in the script — do not trust stale numbers.

First invocation: start with `--dry-run` or `--filter=<substring>`, and expect minutes of build
time on a cold cache — even `--dry-run` builds every discovered fuzzer, which pulls in editor and
renderer deps. An unfiltered run fuzzes every target for the per-fuzzer time budget.

```sh
# Default run (defaults per --help):
python3 tools/fuzzing/run_continuous_fuzz.py

# Longer run: 8 workers, 15 min/fuzzer, 1 h total cap, auto-minimize afterward:
python3 tools/fuzzing/run_continuous_fuzz.py --workers=8 --fuzzer-time=900 \
    --max-total-time=3600 --minimize

# Restrict to fuzzers whose name contains a substring:
python3 tools/fuzzing/run_continuous_fuzz.py --filter=svg_parser
```

Corpus lifecycle after a run (order matters — minimize before update-intree, or you commit
redundant inputs):

```sh
python3 tools/fuzzing/manage_corpus.py minimize --latest   # libFuzzer -merge=1 dedup into ~/.donner-fuzz/corpus
python3 tools/fuzzing/manage_corpus.py update-intree       # copy persistent corpus into the in-tree *_corpus dirs
python3 tools/fuzzing/manage_corpus.py stats               # corpus sizes per fuzzer
```

`update-intree` prunes in-tree files absent from the minimized corpus; pass `--no-prune` to only
add. Review the resulting `git diff` before committing corpus churn.

Crash triage (dedups by top-5 stack-frame signature; ledger in
`~/.donner-fuzz/known_crashes.json`):

```sh
python3 tools/fuzzing/crash_reporter.py report --latest --dry-run   # ALWAYS dry-run first
python3 tools/fuzzing/crash_reporter.py report --latest             # files GitHub issues via gh
python3 tools/fuzzing/crash_reporter.py list                        # known-crash ledger
python3 tools/fuzzing/dashboard.py                                  # summary of recent runs (--json for machine output)
```

Docker deployment (long-lived container that re-fuzzes `main` every 30 min) is documented in
`tools/fuzzing/README.md`; config in `tools/fuzzing/docker-compose.yml`.

## 5. Adding a fuzzer for a new parser

Copy the existing pattern — e.g. `donner/css/parser/BUILD.bazel` (`declaration_list_parser_fuzzer`):

```python
load("//build_defs:rules.bzl", "donner_cc_fuzzer")

donner_cc_fuzzer(
    name = "foo_parser_fuzzer",
    srcs = ["tests/FooParser_fuzzer.cc"],
    corpus = "tests/foo_parser_corpus",   # plain path is globbed; a //label or :rule also works
    deps = [":parser"],
)
```

The source file defines the standard libFuzzer entry point (see
`donner/css/parser/tests/DeclarationListParser_fuzzer.cc` for a minimal example):

```cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = FooParser::Parse(
      std::string_view(reinterpret_cast<const char*>(data), size));
  (void)result;
  return 0;
}
```

Rules of thumb:

- Exercise every public parse entry point of the class in one fuzzer (the declaration-list
  fuzzer calls both `Parse` and `ParseOnlyDeclarations`) — a second entry point left unfuzzed is
  an unguarded crash surface.
- Seed the corpus directory with a handful of small valid inputs (real files, not synthesized
  giants); libFuzzer mutates from there. An empty corpus works but wastes the first hours of
  fuzzing rediscovering the grammar.
- The corpus replay target joins `bazel test //...` on Linux automatically — no extra CI wiring.
- No CMake mirror update needed: `gen_cmakelists.py` skips any target containing `_fuzzer`.
- Naming convention: rule `<subject>_fuzzer`, source `tests/<Class>_fuzzer.cc`, corpus
  `tests/<subject>_corpus`.

## 6. Pitfalls

- **`tools/run_all_fuzzers.sh` is stale — do not use it.** It hardcodes a fuzzer list that
  includes five deleted SMIL fuzzers (`animate_*`, `clock_value_parser`, `syncbase_ref`), omits
  newer ones (`xml_tokenizer`, `bezier_utils`, `path`, `path_ops`, `editor_state_machine`,
  `sandbox_wire`), and assumes prebuilt binaries. Use `run_continuous_fuzz.py` (auto-discovery)
  or the tag-filtered `bazel test` from §2 instead.
- **macOS silent skip** (§2): no `--config=asan-fuzzer` means zero fuzz targets ran, with no
  error. Verify with `--test_tag_filters=fuzz_target` and check the test count is nonzero.
- **Omitting `--build_tag_filters=fuzz_target`** (§2): the command silently becomes a full-repo
  asan-fuzzer build, so an unrelated build break masks every fuzz result as `NO STATUS`.
- **Fixing a crash without committing the input** (§3): the fix is unverified and unguarded.
- **Committing an unminimized corpus**: raw run corpora contain thousands of redundant inputs;
  always route through `manage_corpus.py minimize` before `update-intree`.
- **Sanitizer reports are the bug, not noise.** An ASAN/UBSAN report from a fuzzer on attacker-
  controllable parser input is a real defect in a library that parses untrusted SVG/CSS — fix
  it, never suppress it.

## Related skills

- **donner-bugfix-discipline** — red→green commit sequencing for the crash-fix protocol in §3.
- **donner-build-test** — bazel configs, `bazel test //...` as the pre-push gate, variant lanes.
- **donner-parsers-css-text** — the parser code these fuzzers target.
- **donner-pr-ci** — CI workflows, including where `fuzz.yml` fits relative to `main.yml`.
