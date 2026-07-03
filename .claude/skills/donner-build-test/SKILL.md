---
name: donner-build-test
description: >
  Run, scope, and configure Donner's Bazel build/test surface: choosing a --config, the variant
  lanes (*_tiny/*_text_full/*_geode), authoring BUILD.bazel targets with the donner_* macros,
  sanitizers (asan/ubsan/tsan), coverage, WASM/Emscripten builds, and the CMake mirror. Use when
  running bazel test/build, debugging a *_lint or variant-lane failure, adding or modifying
  BUILD.bazel targets, running the presubmit ritual before pushing a PR, or reading a sanitizer
  report.
---

# Donner Build & Test

Bazel is the primary build system; CMake is a generated mirror for external consumers. For depth
beyond this how-to: `docs/building.md` (setup + FAQ), `build_defs/rules.bzl` (macro docstrings),
`docs/design_docs/0031-ci_hardening_2026q2.md` and `docs/design_docs/0016-ci_escape_prevention.md`
(why the gates exist), `AGENTS.md` §"Pull Request Workflow".

## 1. TL;DR command card

```sh
bazel test //...                      # THE single local gate before any push — nothing else
bazel test //donner/base/...         # scope while iterating (renderer tests are slow)
bazel build //donner/...             # build everything
bazel test --test_tag_filters=lint //...           # just the banned-pattern lint tests
bazel test //donner/svg/renderer/tests:renderer_regression_tests_geode   # one variant lane
```

`bazel test //...` covers, with NO extra flags: unit tests at the default config, the auto-emitted
`*_tiny` / `*_text_full` / `*_geode` variant wrappers, per-target `*_lint` banned-pattern tests,
and fuzzer corpus-replay tests. There is no separate presubmit script — a `tools/presubmit.sh`
wrapper used to exist but was retired; do not reference it.

Timing: with a warm disk/remote cache an incremental `bazel test //...` finishes in minutes; a
cold build compiles thousands of actions (wgpu-native, the editor, the text stack) and can take
tens of minutes to an hour or more depending on hardware. It is building, not hung, as long as
the `[N / M]` action counter in the progress output keeps advancing.

Quiet mode: `.claude/settings.json` sets `LLM=1`, and `.bazelrc` forwards it into tests
(`test --test_env=LLM`). Renderer image-comparison tests then suppress verbose pixel dumps and
terminal previews. Missing diagnostics means quiet mode, not a broken harness — rerun with
`DONNER_RENDERER_TEST_VERBOSE=1 bazel test <target> --test_output=errors` to get full output
(also forwarded via `.bazelrc`).

## 2. Presubmit ritual (exact order)

1. `git fetch origin main && git rebase origin/main`
2. Format changed files (clang-format 18 and 19 produce identical output under the project
   `.clang-format`; bare `git clang-format` only covers uncommitted changes, so pass the branch
   base for already-committed work):
   ```sh
   git-clang-format origin/main                                        # C/C++, whole branch
   git diff --name-only origin/main | grep -E '(BUILD|\.bzl)$' | xargs -r buildifier
   ```
3. `bazel test //...` — must be fully green. There is no "preexisting failure": any red test is
   now in scope to fix (CLAUDE.md §"Always-Green Main").
4. If you touched `tools/cmake/` or the dependency graph:
   `python3 tools/cmake/gen_cmakelists.py --check --build` — plain `--check` is static-only and
   misses real compile breaks; `--build` is the local compile gate (`--build` requires `--check`).
5. Fuzzer-sensitive changes — any change under a package whose BUILD.bazel has a
   `donner_cc_fuzzer` target (grep the directory): `bazel test --config=asan-fuzzer <fuzzer target>` (see donner-fuzzing skill). Required on macOS because Apple Clang lacks
   `libclang_rt.fuzzer_osx.a`; the config activates the LLVM toolchain that provides it.

PR/CI mechanics (bazel-diff target determinator, `ci:full-test` label, CI monitoring cadence)
live in the donner-pr-ci skill.

## 3. `--config` cheat sheet (all verified in `.bazelrc`)

| Config                                                    | What it does                                                                                                                                                                                        |
| --------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| _(none)_                                                  | tiny-skia backend, basic text ON (stb_truetype), filters ON                                                                                                                                         |
| `tiny-skia`                                               | Explicitly select the tiny-skia backend (same as default)                                                                                                                                           |
| `geode`                                                   | Geode GPU backend (WebGPU); also sets `--//donner/svg/renderer/geode:enable_geode=true` so wgpu-native compiles                                                                                     |
| `tiny`                                                    | filters=false, text=false (the "donner-tiny" product tier)                                                                                                                                          |
| `text-full`                                               | FreeType + HarfBuzz + WOFF2 text (the "donner-max" tier)                                                                                                                                            |
| `no-text` / `no-filters`                                  | Disable one feature axis                                                                                                                                                                            |
| `dev`                                                     | Disables the renderer-backend test transitions — use when iterating with an explicit `--config` so variant wrappers don't build duplicate configurations                                            |
| `no-perf-opt`                                             | Sets `--//build_defs:disable_perf_opt_transition=true` — inert today: perf-sensitive libs no longer transition (see §5); the flag is only read by the unused `_force_opt_transition` in `rules.bzl` |
| `debug`                                                   | `-c dbg -O0 -g`, dSYMs, standalone spawn strategy                                                                                                                                                   |
| `latest_llvm`                                             | Hermetic LLVM toolchain (Linux exec platforms); required for coverage, pulled in automatically by sanitizer configs                                                                                 |
| `asan` / `ubsan` / `tsan`                                 | Sanitizers, see §6 (each pulls `latest_llvm`)                                                                                                                                                       |
| `asan-fuzzer`                                             | ASan + libFuzzer engine; mandatory for fuzzers on macOS                                                                                                                                             |
| `coverage`                                                | Coverage instrumentation, see §7 (`bazel coverage` applies it automatically)                                                                                                                        |
| `time-trace`                                              | Clang `-ftime-trace` compile profiling; forces local standalone execution, then `python3 tools/time_trace_report.py` aggregates                                                                     |
| `binary-size` / `linux-binary-size` / `macos-binary-size` | `-Os -g` size-analysis builds                                                                                                                                                                       |
| `wasm` / `wasm-geode` / `editor-wasm`                     | Emscripten builds, see §8 (`editor-wasm-geode` is a back-compat alias for `editor-wasm`)                                                                                                            |
| `clang-tidy`                                              | Runs clang-tidy as a Bazel aspect (`--output_groups=report`)                                                                                                                                        |
| `ci` / `re`                                               | CI/remote-execution only — do not use locally, see §10                                                                                                                                              |

## 4. Variant-lane mechanics

`donner_cc_test(variants = ["tiny", "text_full", "geode"])` in `build_defs/rules.bzl` emits one
transitioned `{name}_{variant}` wrapper per entry via `donner_multi_transitioned_test`, plus the
plain `{name}` target which inherits whatever config is on the command line. The specs
(`_VARIANT_SPECS`): `tiny` = tiny_skia/no text, `text_full` = tiny_skia/full text, `geode` =
geode backend/basic text. Each wrapper is tagged `variant_<name>`, so
`--test_tag_filters=variant_geode` selects a lane.

CRITICAL: wrappers only copy the attrs in `_VARIANT_FORWARDED_ATTRS` — `size`, `timeout`,
`shard_count`, `flaky`, `local`, `target_compatible_with`. But two base-target attrs still take
effect at runtime through other channels: `data` (the wrapper reuses the base test's runfiles)
and `env`/`env_inherit` (the wrapper rule forwards `RunEnvironmentInfo`; see
`_donner_transitioned_executable_impl` — this is how the resvg suite's
`DONNER_TEST_TIMEOUT_SECONDS` watchdog override reaches its `_geode` lane). What genuinely does
NOT propagate: `args`, and any `tags` beyond the auto-added `variant_<name>`.

`donner_variant_cc_test(name, dep, named_variants = [...])` is the explicit-dict form for suites
whose variants don't match the standard specs — the resvg suite
(`donner/svg/renderer/tests/BUILD.bazel`, target `resvg_test_suite`) uses it to emit
`resvg_test_suite_default_text` / `_max` / `_geode` around an impl target gated by
`target_compatible_with` on `//donner/svg/renderer:resvg_test_suite_enabled` (not `manual`
tags — the `rules.bzl` docstring saying "tagged manual" is stale).

## 5. Target authoring (BUILD.bazel)

Never use raw `cc_library`/`cc_test`/`cc_binary` under `donner/` — the `donner_*` macros from
`//build_defs:rules.bzl` add the pieces CI relies on:

| Macro                              | Adds                                                                                                                                                                                                                                                       |
| ---------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `donner_cc_library`                | `include_prefix`, `-I.`, `{name}_lint` test (no libc-compat deps — those are test/binary-only). Only legal under `donner/` or `experimental/` (the macro `fail()`s elsewhere; experimental targets are auto-tagged and must stay `//experimental`-visible) |
| `donner_cc_test`                   | macOS sanitizer-runtime rpaths, libc-compat deps, `{name}_lint`, optional `variants`                                                                                                                                                                       |
| `donner_cc_binary`                 | same defaults as the test macro, plus `{name}_lint`                                                                                                                                                                                                        |
| `donner_perf_cc_test`              | emits `{name}_correctness` (PR-gated) + `{name}_wallclock` (tagged `manual` + `perf`, nightly only) — see donner-bugfix-discipline                                                                                                                         |
| `donner_cc_fuzzer`                 | three targets per fuzzer — see donner-fuzzing                                                                                                                                                                                                              |
| `donner_perf_sensitive_cc_library` | compiles its own srcs optimized by requesting the `opt` feature via `cc_common` — deps' configuration is unchanged, so no cross-config duplicate symbols (it is NOT a transition; `--config=no-perf-opt` does not affect it)                               |

Minimal worked example (every BUILD.bazel needs the `load()`s; libraries use the shared
visibility helper):

```python
load("//build_defs:rules.bzl", "donner_cc_library", "donner_cc_test")
load("//build_defs:visibility.bzl", "donner_internal_visibility")

donner_cc_library(
    name = "base64",
    srcs = ["Base64.cc"],
    hdrs = ["Base64.h"],
    visibility = donner_internal_visibility(),   # == ["//donner:__subpackages__"]
    deps = ["//donner/base"],
)

donner_cc_test(
    name = "base64_tests",
    srcs = ["tests/Base64_tests.cc"],
    variants = ["tiny", "text_full", "geode"],   # only if behavior depends on tier/backend
    deps = [":base64", "@com_google_gtest//:gtest_main"],
)
```

Rules of thumb:

- Add `variants = [...]` to any test whose behavior depends on text tier or renderer backend —
  nobody runs `--config` lanes manually on PRs, so an un-varianted test silently loses coverage.
- Backend-only targets: `target_compatible_with = renderer_backend_compatible_with(["geode"])`
  (from `rules.bzl`); other configs then skip the target instead of failing to compile.
- Runtime feature guards inside tests: `ActiveRendererBackend()` /
  `ActiveRendererSupportsFeature(...)` from `donner/svg/renderer/tests/RendererTestBackend.h`,
  paired with `GTEST_SKIP()`.
- The `{name}_lint` py_test runs `build_defs/check_banned_patterns.py` over the target's
  srcs+hdrs (tags: `lint`, `banned_patterns`). Reproduce a failure locally with
  `python3 build_defs/check_banned_patterns.py <files>`; fix the SOURCE, never the lint (see
  donner-cpp-conventions for the banned-pattern list and rationale).

## 6. Sanitizers

```sh
bazel test --config=asan //donner/...    # ASan + UBSan combined (-fsanitize=address,undefined)
bazel test --config=ubsan //donner/...   # UBSan only
bazel test --config=tsan //donner/...    # ThreadSanitizer
```

Facts that matter when reading reports (all set in `.bazelrc`):

- `--config=asan` enables BOTH address and undefined-behavior checks; `vptr` and `function`
  checks are disabled in all sanitizer configs (libubsan linking issue), so absence of those
  report types is expected, not a pass.
- `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1` — UBSan failures abort the test with a
  stack trace rather than printing-and-continuing, so one red test = the first UB hit.
- `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1` — deadlock reports include both stacks.
- All three pull `--config=latest_llvm` (hermetic toolchain) and `--strip=never`, so frames
  symbolize. If ASan frames are bare addresses, export `ASAN_SYMBOLIZER_PATH` (the `.bazelrc`
  forwards it into tests via `--test_env`).
- There are NO sanitizer suppression files checked into this repo — every report is actionable.
  Do not add suppressions to make a lane green; fix the bug.
- A memory-safety report in parser code (XML/SVG/CSS) is security-relevant: after the
  red→green fix (donner-bugfix-discipline), also run that parser's fuzzer over the fix and add
  the triggering input to the fuzzer corpus — see donner-fuzzing §3's crash-fix protocol.
  Sanitizer findings and fuzzer findings are the same bug class caught by different harnesses.

CI: `sanitizers.yml` runs `--config=asan` and `--config=ubsan` over `//donner/...` nightly (with
a gatekeeper that skips idle days). `sanitizers-pr.yml` runs an informational, non-blocking
`--config=asan --config=geode //donner/svg/renderer/tests:resvg_test_suite_geode` on PRs touching
Geode renderer paths (kept non-blocking per doc 0031 M1.2 while issue #552's fix is in flight).

## 7. Coverage

```sh
tools/coverage.sh                  # full run: bazel coverage + filter + HTML
tools/coverage.sh --quiet --no-html //donner/base/...   # scoped, LCOV only
```

- The script runs `bazel coverage --config=latest_llvm <targets>` (the `coverage` command
  auto-applies `--config=coverage` from `.bazelrc`), filters the combined report with
  `tools/filter_coverage.py`, and writes HTML to `coverage-report/index.html`.
- Local prerequisites the script checks for: `genhtml` (install lcov), a Java runtime (bazel-diff
  style jar use inside bazel coverage tooling), and `llvm-cov`/`llvm-profdata` reachable via
  `clang --print-prog-name=...`.
- Defaults to `//donner/...`; fuzz and lint tests are excluded via
  `--test_tag_filters=-fuzz_target,-lint` in `.bazelrc`.
- Coverage test results ARE cacheable (`--experimental_fetch_all_coverage_outputs` keeps cached
  runs' coverage.dat in the combined report — validated 2026-07-02). Never re-add
  `--nocache_test_results` to the coverage config; it forced every CI coverage run to re-execute
  every test and dominated wall time.

## 8. WASM / Emscripten

Three configs (`.bazelrc` also sets `disable_perf_opt_transition` on them — historical, inert
today; see the `no-perf-opt` row in §3):

| Config        | Backend                                                    | Verified build command                                                         |
| ------------- | ---------------------------------------------------------- | ------------------------------------------------------------------------------ |
| `wasm`        | tiny-skia, text off                                        | `bazel build --config=wasm //donner/svg/renderer/wasm:donner_wasm`             |
| `wasm-geode`  | Geode via emdawnwebgpu browser WebGPU bindings (+ASYNCIFY) | `bazel build --config=wasm-geode //donner/svg/renderer/wasm:donner_wasm_geode` |
| `editor-wasm` | full editor; = `wasm-geode` + editor flag + `-pthread`     | see below                                                                      |

Editor-in-browser, local build + serve (from `docs/building.md`, targets in
`donner/editor/wasm/BUILD.bazel`):

```sh
bazel build --config=editor-wasm //donner/editor/wasm:wasm_web_package
bazel run   --config=editor-wasm //donner/editor/wasm:serve_http
bazel run   --config=editor-wasm //donner/editor/wasm:serve_http -- --https   # LAN + local cert
```

Then open the served `index.html`. CI parity: `editor_wasm.yml` builds
`--config=editor-wasm //donner/editor/wasm:wasm_web_package` nightly on ubuntu-24.04.

Gating: all wasm targets are `target_compatible_with`-gated on an `enable_wasm` flag that only
the wasm configs set, so plain `bazel build //...` never touches them — and a bazel-diff target
list that names them on a non-wasm host is why `--config=ci` carries
`--skip_incompatible_explicit_targets`.

## 9. CMake mirror

Generated, not hand-maintained: `**/CMakeLists.txt` is gitignored (only `examples/` checks
CMake files in). Commands:

```sh
python3 tools/cmake/gen_cmakelists.py                    # regenerate (runs bazel query; run
                                                         # from the workspace, outside bazel)
python3 tools/cmake/gen_cmakelists.py --check            # static validation, fast
python3 tools/cmake/gen_cmakelists.py --check --build    # + real cmake configure/compile gate
bazel test //tools/cmake:gen_cmakelists_test             # generator unit tests
cmake -S . -B build -DDONNER_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
```

A new `bazel_dep` in `MODULE.bazel` usually needs a mapping in `tools/cmake/external_deps.json`,
otherwise `--check` reports an unmapped external dep. `examples/cmake_consumer/` is the
standalone consumer project for the exported `donner` target.

## 10. Failure signatures → meaning

| Signature                                                       | Meaning / action                                                                                                                                                                                                                                                         |
| --------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `{target}_lint` red                                             | Banned pattern in source. `python3 build_defs/check_banned_patterns.py <files>` to reproduce; fix the source (donner-cpp-conventions)                                                                                                                                    |
| `*_geode` wrapper red, default lane green                       | Real backend divergence. "Anti-aliasing" is a banned explanation (CLAUDE.md); see donner-geode-backend + donner-pixel-diff                                                                                                                                               |
| Duplicate-symbol link error mentioning two configurations       | Two configurations of the same dep in one link — find the transition that duplicated it (e.g. a variant wrapper mixing transitioned and untransitioned deps). `--config=no-perf-opt` is NOT the fix: perf-sensitive libs no longer transition (§5), so the flag is inert |
| Fuzzer link error on macOS (`libclang_rt.fuzzer_osx.a` missing) | Build with `--config=asan-fuzzer` (activates hermetic LLVM)                                                                                                                                                                                                              |
| Geode tests crash/hang on Intel Arc Xe hosts                    | Force llvmpipe: `--test_env=VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json --test_env=XDG_RUNTIME_DIR=/tmp` (AGENTS.md)                                                                                                                                           |
| A `*_wallclock` perf test red on a PR                           | Should not be PR-gated: `test:ci --test_tag_filters=-perf` in `.bazelrc` keeps wall-clock budgets on nightly `perf.yml`. Never remove that filter                                                                                                                        |
| Renderer test failed but printed no pixel diff                  | Quiet mode (`LLM=1`); rerun with `DONNER_RENDERER_TEST_VERBOSE=1`                                                                                                                                                                                                        |
| IDE reports headers not found but bazel builds fine             | Stale compile_commands.json → `tools/refresh_compile_commands.sh`; trust `bazel build`                                                                                                                                                                                   |
| `@resvg-test-suite` resolution failure in a downstream consumer | Expected: the suite lives in the dev-only `non_bcr_deps` module extension; `.bazelrc` forces `--//donner/svg/renderer:resvg_test_suite_available=true` for local/CI only                                                                                                 |

## 11. Hard rules

- Any red in `bazel test //...` is in scope to fix now (§2 step 3).
- Never add `--jobs` / `--local_test_jobs` to the `ci` config in `.bazelrc`: self-hosted runners
  cap concurrency in `/etc/bazel.bazelrc`, and overriding it flooded the remote-execution worker
  (2026-06-30 hang; see the comment above `build:ci`).
- Slow compiles are a measurable bug: `bazel build --config=time-trace <targets>` then
  `python3 tools/time_trace_report.py` — don't guess at header costs.
