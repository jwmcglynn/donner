---
name: donner-resvg-triage
description: Run and triage Donner's resvg SVG-conformance suite — filtering tests, reading [COMPARE] failures, editing Params::Skip/RenderOnly/threshold entries in resvg_test_suite.cc, and handling the per-backend comparison modes. Use when a resvg_test_suite test fails, when enabling tests for a newly implemented SVG feature, when adding/removing skip entries or thresholds, when running a DISABLED_ (skipped) comparison, or when touching donner/svg/renderer/tests/resvg_test_suite.cc or third_party/resvg-test-suite.
---

# Resvg test-suite triage

`//donner/svg/renderer/tests:resvg_test_suite` renders every `.svg` in the vendored
[resvg-test-suite](https://github.com/RazrFalcon/resvg-test-suite) (at `third_party/resvg-test-suite/`:
`tests/`, `fonts/`, `resources/`) and pixel-diffs the output against resvg's golden PNGs using
pixelmatch. It is the acceptance suite for SVG conformance work: identify the relevant tests
_before_ implementing a feature, use them as red→green evidence, and record every known gap through
the `Params` API in `donner/svg/renderer/tests/resvg_test_suite.cc`.

Key files:

- `donner/svg/renderer/tests/resvg_test_suite.cc` — one `INSTANTIATE_TEST_SUITE_P` block per
  category; all skips/thresholds live here.
- `donner/svg/renderer/tests/ImageComparisonTestFixture.h/.cc` — `ImageComparisonParams` builder
  API, comparison modes, name mangling. The authoritative source for what each Param does.
- `donner/svg/renderer/tests/README_resvg_test_suite.md` — full triage walkthrough with output
  anatomy (mostly current; stale spots called out below).
- `docs/design_docs/0021-resvg_feature_gaps.md` — living catalog of every skip/threshold with root
  cause and next step. When you fix a gap, delete its entry there and un-skip in the same PR.
  The doc's `F#`/`B#` IDs do not appear in `resvg_test_suite.cc` — find the matching entry by
  grepping the doc for the `Params` reason string's key words (e.g. "primitive subregion" → F8).
  `docs/design_docs/0009-resvg_test_suite_bugs.md` — cases where resvg's golden is wrong (golden
  overrides).

## Running the suite

```sh
bazel run //donner/svg/renderer/tests:resvg_test_suite            # default config (tiny-skia)
bazel run -c dbg //donner/svg/renderer/tests:resvg_test_suite -- '--gtest_filter=*<name>*'
bazel test //donner/svg/renderer/tests:resvg_test_suite_default_text --test_filter='*<name>*'
```

The bare `:resvg_test_suite` name is a plain alias that inherits your command-line config — it
works with `bazel run` / `bazel build`, but **`bazel test` on it finds zero tests** ("No test
targets were found"): Bazel's `tests()` expansion does not follow a bare alias. For `bazel test`
use `:resvg_test_suite_impl` or one of the variant targets. The `donner_variant_cc_test` macro
generates self-transitioning variant targets that run under plain `bazel test //...` with no
extra flags:

- `:resvg_test_suite_default_text` — tiny-skia, simple text (Donner default build)
- `:resvg_test_suite_max` — tiny-skia, full text (FreeType + HarfBuzz)
- `:resvg_test_suite_geode` — Geode (WebGPU) backend

So "make resvg green" means all three variants, and `bazel test //...` is the final gate
(see the donner-build-test and donner-pr-ci skills). To point the _alias_ at a non-default config
use `--config=text-full` or `--config=geode` (defined in `.bazelrc`); the variant targets need no
config flag because they transition themselves.

### Test names and filtering

Full gtest name: `<SuiteName>/ImageComparisonTestFixture.ResvgTest/<sanitized>` where
`<sanitized>` is the `.svg` filename stem with every non-alphanumeric character replaced by `_`
(`zero-length-path-with-round.svg` → `zero_length_path_with_round`; verified against the current
`third_party/resvg-test-suite/tests` tree, 2026-07-02 — upstream renames files, so check the tree
if a filter matches nothing). Use a wildcard filter (`'*zero_length_path_with_round*'`) so you
don't have to reconstruct the suite prefix.

Bare filenames repeat across categories (e.g. `with-subregion-on-input-1.svg` exists in both
feBlend and feComposite blocks), so one wildcard filter can match several suites. Before editing an
override entry, check the suite-name prefix in the test output (`FiltersFeBlend/...` vs
`FiltersFeComposite/...`) to pick the right category block.

On multi-mode builds (Geode) a mode suffix is appended: `..._TinyGolden` / `..._GeodeGolden`.
Single-mode (CPU) builds keep the bare historical names — a filter ending in the sanitized stem
matches CPU builds but NOT geode builds; end with `*` to match both.

### Running a skipped test

`Params::Skip(...)` (and `.onlyTextFull()` on non-text-full builds) emits the test with a
`DISABLED_` name prefix. Run it WITHOUT editing the skip table — temporarily deleting a skip entry
is how skip removals get accidentally committed:

```sh
bazel run -c dbg //donner/svg/renderer/tests:resvg_test_suite -- \
  '--gtest_filter=*<sanitized-name>*' --gtest_also_run_disabled_tests
```

Text-full-only tests (`.onlyTextFull()`) still need a text-full build (`--config=text-full` or the
`_max` variant) — on simple-text builds they are DISABLED regardless of the flag.

## Reading a failure

Per-comparison output line (`ImageComparisonTestFixture.cc`):

```
[  COMPARE ] .../tests/text/word-spacing/simple-case.svg [TinySkia]: FAIL (8234 pixels differ, with 100 max)
```

- `[TinySkia]` / `[Geode]` — which backend rendered the "actual" image.
- `8234 pixels differ` — pixelmatch mismatch count (anti-aliased edge pixels already excluded).
- `with 100 max` — the test's `maxMismatchedPixels` budget (default `kDefaultMismatchedPixels` =
  100; default per-pixel threshold `kDefaultThreshold` = 0.02).

On failure the fixture writes the actual render and diff PNGs and prints their absolute paths:

```
Actual rendering: <path>.png
Expected: <golden path>.png
Diff: <path>/diff_....png
```

**Copy the printed paths** — they go to `TEST_TMPDIR` under `bazel test` and the system temp dir
under `bazel run` (see `donner/base/tests/TestTempDir.h`). The README's claim that they land in
`$TEST_UNDECLARED_OUTPUTS_DIR` is stale — trust the printed paths. `Expected:` is the committed
golden, which sits next to its `.svg` in the same category directory with a `.png` extension.

Verbosity: `LLM=1` (set by `.claude/settings.json` and forwarded by `test --test_env=LLM` in
`.bazelrc`) suppresses the verbose backend re-render and the SVG source echo — NOT the terminal
preview, despite what the fixture's own hint text claims. The ANSI terminal-image preview is gated
separately: `DONNER_ENABLE_TERMINAL_IMAGES=0` disables it (default on; per-test default
`params.showTerminalPreview = true`). Re-enable verbose output with
`DONNER_RENDERER_TEST_VERBOSE=1` when you need instantiation/transform logs and the inline SVG
source. The failure output prints a rerun hint with a literal `<target>` placeholder — substitute
the resvg target and your `--gtest_filter` yourself.

Startup noise: repeated `error at 0:0: Data corrupted` lines appear on every run — all categories
register at binary startup regardless of `--gtest_filter`, and `filters/filter-functions` produces
this known resource-loading noise (TODO in `resvg_test_suite.cc`). Ignore them unless your test is
in that category.

## Comparison modes (verified against ActiveComparisonModes())

`ActiveComparisonModes()` in `ImageComparisonTestFixture.cc` returns:

- CPU builds: `{ TinyGolden }` — tiny-skia render vs the committed golden.
- Geode builds: `{ TinyGolden, GeodeGolden }` — each backend vs the same committed golden.

**`GeodeTinyParity` (geode-render-vs-tiny-render) is retired** and no longer runs; the enum member
and `.disableGeodeParity(...)` builder still exist but are inert in this suite.
`README_resvg_test_suite.md` still claims three modes — that is stale; the `.cc` implementation
(`ActiveComparisonModes()`) is authoritative.

A test can pass `_TinyGolden` and fail `_GeodeGolden` (or vice versa). Handle per-backend
divergence with, in order of preference:

1. Fix the backend bug (default answer).
2. `.disableBackend(RendererBackend::Geode, "reason")` — for features one backend doesn't
   implement yet; the other backend still gates.
3. `.withGeodeMaxPixelsDifferent(px)` — a Geode-mode-only mismatch budget; `TinyGolden` keeps the
   strict budget, so the healthy backend is not blinded (e.g. `control-points-clamping-1.svg` in
   the `painting/stroke` block: 150 px shared, 500 px on Geode).
4. `.withGeodeGoldenOverride("donner/svg/renderer/testdata/golden/geode/<name>.png", "reason")` —
   ONLY for a verified-correct Geode output that differs from the shared golden by a genuine
   sub-pixel analytic difference (e.g. 8-bit filter-intermediate precision). TinyGolden still uses
   the shared golden.

Never widen the shared threshold to absorb a one-backend diff — that blinds the healthy backend.

## Triage decision tree

1. **Diff ≤ 100 px** → passes automatically; NO entry needed. Never add `{"x.svg", Params()}`
   no-op entries — they are review-rejected churn.
2. **Feature not implemented** (element/attribute missing) →
   `Params::Skip("Not impl: <feature>")`.
3. **Golden is UB** (resvg's golden PNG carries a "UB" text overlay, or the case is
   deprecated/undefined behavior) → `Params::RenderOnly("UB: <reason>")` — renders for no-crash
   coverage but skips the comparison. (AGENTS.md's "UB → always `Params::Skip()`" line is stale;
   the suite uses `RenderOnly`, e.g. `Params::RenderOnly("saturate 99999 (UB)")`.)
4. **Known bug, fix deferred** → `Params::Skip("Bug: <description>")` and record it in
   `docs/design_docs/0021-resvg_feature_gaps.md`.
5. **Unexplained diff of any size** → investigate. "It's just anti-aliasing" is a banned
   conclusion: pixelmatch already excludes AA edge pixels before counting, so any reported diff
   has a real cause (wrong transform, coverage geometry, color space, premultiplication,
   compositing). Magnitude is the tell — hundreds of pixels, per-glyph drift, or whole-shape
   offsets are never edge fringe. See the donner-pixel-diff skill.
6. **Threshold change** — `Params::WithThreshold(threshold, maxPx, "reason")` ONLY after a
   root-cause investigation, always with the reason string, and **only with explicit human
   approval — an agent may propose a non-default threshold but never lands one on its own**
   (AGENTS.md: "Threshold changes are a last resort requiring explicit human approval"). Widening
   a threshold to absorb an unexplained diff is masking a bug, not fixing one.

Reason strings go in the `Params` argument, not trailing `//` comments — they surface in gtest
skip messages and failure logs, so they are discoverable without opening the source.

## Editing resvg_test_suite.cc

Registration helper (verified; AGENTS.md's `getTestsWithPrefix` name is stale):

```cpp
std::vector<ImageComparisonTestcase> getTestsInCategory(
    std::string_view category,                                   // e.g. "painting/stroke-linecap"
    std::map<std::string, ImageComparisonParams> overrides = {}, // keyed by BARE filename + ext
    ImageComparisonParams defaultParams = {});
```

It scans one directory under `third_party/resvg-test-suite/tests/<category>/` and registers every
`.svg`. Files not in `overrides` get `defaultParams`. A feature spanning categories is therefore
tracked once per category block, not in a shared list. Beware: a typo'd category name returns an
empty vector _silently_ — zero tests, no error — so after adding a block, confirm the tests appear
with a `--gtest_filter` run. Category-wide requirements are automatic: `filters/*` requires the
FilterEffects backend feature, `text/*` requires Text; canvas is forced to 500x500 to match resvg's
references.

Group override entries by reason with a short comment line, e.g.:

```cpp
{
    // Not impl: primitive subregion clipping
    {"with-subregion-on-input-1.svg", Params::Skip("Not impl: primitive subregion clipping")},
    {"with-subregion-on-input-2.svg", Params::Skip("Not impl: primitive subregion clipping")},
}
```

`ImageComparisonParams` builder catalog (verified in `ImageComparisonTestFixture.h` — re-check the
header when in doubt, it is the source of truth):

- Static constructors: `Params::Skip(reason)`, `Params::RenderOnly(reason)`,
  `Params::WithThreshold(threshold, maxPx, reason)`,
  `Params::WithGoldenOverride(filename, threshold, reason)`.
- Chainable: `.withReason(text)`, `.withMaxPixelsDifferent(px)`, `.withSimpleTextMaxPixels(px)`
  (looser budget only when built without HarfBuzz), `.withGeodeMaxPixelsDifferent(px)` (looser
  budget only for Geode comparison modes), `.onlyTextFull()`, `.setCanvasSize(w, h)`,
  `.disableBackend(backend, reason)`, `.requireFeature(feature, reason)`,
  `.withGeodeGoldenOverride(filename, reason)`, `.includeAntiAliasingDifferences()`,
  `.enableGoldenUpdateFromEnv()`, `.disableGeodeParity(reason)` (inert — parity mode retired).

After editing, verify: rerun with a category filter and confirm skips show as skipped, passes
still pass, and the un-skipped test actually runs. Then run `bazel test //...` before pushing
(always-green-main rule; the geode/text-full variants run there too).

Counts of skips/thresholds are volatile — derive them, don't quote docs:

```sh
grep -c 'Params::Skip'       donner/svg/renderer/tests/resvg_test_suite.cc
grep -c 'Params::RenderOnly' donner/svg/renderer/tests/resvg_test_suite.cc
grep -c 'WithThreshold'      donner/svg/renderer/tests/resvg_test_suite.cc
ls third_party/resvg-test-suite/tests/            # current category roots
```

## Golden updates

Goldens are resvg's reference renders — you normally never regenerate them from Donner output.
`UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace)` only takes effect for tests with
`.enableGoldenUpdateFromEnv()` or when a golden file is missing (e.g. a new
`.withGeodeGoldenOverride` PNG). Committing a Donner render over a resvg golden converts a
conformance test into a self-fulfilling snapshot — if you believe resvg's golden is wrong, that
claim belongs in `docs/design_docs/0009-resvg_test_suite_bugs.md` with a `WithGoldenOverride`.

## Batch triage MCP server (optional)

For bulk failures (50+), `tools/mcp-servers/resvg-test-triage/` provides an MCP (Model Context
Protocol) server that parses test output, groups failures by detected SVG feature, and generates
consistently formatted skip entries. Setup: `pip install -e tools/mcp-servers/resvg-test-triage`
(config example: `tools/mcp-servers/resvg-test-triage/mcp-config-example.json`; also preconfigured
in the repo's `.vscode/mcp.json`). Tools include `batch_triage_tests`, `analyze_test_failure`,
`detect_svg_features`, `suggest_skip_comment`, `suggest_implementation_approach`,
`find_related_tests`, `generate_feature_report`, `analyze_visual_diff` — see its `README.md`.

## Related skills

- donner-pixel-diff — pixelmatch rules, no percentage thresholds, "AA is never the root cause".
- donner-bugfix-discipline — red→green requirements before claiming a resvg test fixed.
- donner-rendering-pipeline / donner-geode-backend — where to fix the actual rendering bug.
- donner-build-test — variants, configs, and the `bazel test //...` gate.
