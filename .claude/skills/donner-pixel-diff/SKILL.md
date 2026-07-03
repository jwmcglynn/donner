---
name: donner-pixel-diff
description: >-
  Author, triage, and update bitmap/golden-image comparisons in Donner using the one blessed
  comparator (bitmap_golden_compare / ImageComparisonTestFixture + pixelmatch), read diff PNGs, and
  regenerate goldens safely. Use when a golden-image or pixel-diff test fails (output mentions
  "pixels differ", actual_*.png / diff_*.png, or UPDATE_GOLDEN_IMAGES_DIR), when writing any new
  bitmap comparison (editor replay, layer thumbnails, renderer goldens), or when regenerating
  goldens after an intentional rendering change.
---

# Donner Pixel-Diff and Golden-Image Tests

All bitmap comparisons in Donner go through pixelmatch (pixelmatch-cpp17) via one of two blessed
helpers. Never roll your own comparator. Repo root is the Bazel workspace; all paths below are
workspace-relative.

## The two blessed comparators

| Layer          | Helper                                        | Location                                                 |
| -------------- | --------------------------------------------- | -------------------------------------------------------- |
| Editor tests   | `//donner/editor/tests:bitmap_golden_compare` | `donner/editor/tests/BitmapGoldenCompare.h`              |
| Renderer tests | `ImageComparisonTestFixture`                  | `donner/svg/renderer/tests/ImageComparisonTestFixture.h` |

### Editor: `bitmap_golden_compare`

- `CompareBitmapToGolden(bitmap, goldenPath, testLabel, params)` — compare a `RendererBitmap`
  against a committed PNG (path relative to workspace, e.g.
  `"donner/editor/tests/testdata/layer_thumbnails/donner_splash_donner.png"`).
- `CompareBitmapToBitmap(actual, expected, testLabel, params)` — when ground truth is another live
  code path (e.g. replay output vs. a direct `svg::Renderer` render) rather than a committed file.
- Params (`BitmapGoldenCompareParams`): default is `threshold = 0.02`, `maxMismatchedPixels = 100`,
  `includeAntiAliasing = false`. **New tests must use `PixelmatchIdentityParams()`** (threshold 0,
  0 mismatched pixels allowed, anti-aliased pixels counted too). Any looser tolerance must go
  through `ApprovedPixelToleranceParams(threshold, maxMismatchedPixels)` — that name exists so a
  reviewer can grep for every approved exception; add a comment explaining why identity was
  rejected.
- Golden PNGs must be listed in the test target's `data = [...]` in `BUILD.bazel` (usually a
  `glob(["testdata/.../*.png"])`), or the test fails with "could not load golden PNG".

### Renderer: `ImageComparisonTestFixture`

- `renderAndCompare(document, svgFilename, goldenFilename, params)` with
  `ImageComparisonParams` (defaults: threshold `0.02`, `100` mismatched pixels, AA excluded).
  Builders: `ImageComparisonParams::WithThreshold(threshold, maxPixels)`,
  `.includeAntiAliasingDifferences()`, `.setCanvasSize(w, h)`, `.enableGoldenUpdateFromEnv()`.
- Under a Geode (WebGPU/GPU backend) build, each golden test runs two parameterized instances
  (the mode appears as a test-name suffix): `TinyGolden` (tiny-skia CPU render vs the shared
  golden) and `GeodeGolden` (GPU render vs the same golden). The third mode, `GeodeTinyParity`
  (GPU render vs an in-process tiny-skia render), is retired — `ActiveComparisonModes()` in
  `ImageComparisonTestFixture.cc` never returns it (the enum member and dump code still exist).
  A Geode-only mismatch budget is set with `.withGeodeMaxPixelsDifferent(px)`; `TinyGolden`
  keeps the strict budget. Geode-wide slack and per-test exceptions live in `geodeOverrides()`
  in `donner/svg/renderer/tests/Renderer_tests.cc` — every entry is a divergence to root-cause,
  not a permanent waiver.

### Banned patterns (and why)

- **No private comparators.** Hand-rolled `composeOver` / `CompositeOver` /
  `CountDifferingPixelsInRect` helpers inside a test file are banned: a boutique comparator with a
  5% threshold hid bug #582 for weeks. If you need composition or cropping the helper lacks,
  extend `bitmap_golden_compare` itself so every test benefits.
- **No percentage thresholds.** They mask regressions smaller than the threshold and scale with
  scene size. Either the diff is zero, or the test writes inspectable PNGs (below) and fails.
- See CLAUDE.md §"Pixel-Diff Tests" for the policy statement.

## Inspecting a failure

On mismatch the helpers write PNGs named from the golden path or test label (slashes flattened to
underscores). Naming and location differ per helper — read the failure message's printed paths
first; they are authoritative.

- **Editor helper** (`bitmap_golden_compare`): `actual_<name>.png`, `expected_<name>.png`,
  `diff_<name>.png`, and `side_by_side_<name>.png` (expected left, actual right). Written to
  `$TEST_UNDECLARED_OUTPUTS_DIR`, so under `bazel test` they surface in
  `bazel-testlogs/<package>/<target>/test.outputs/outputs.zip` (sharded targets add a
  `shard_N_of_M/` level).
- **Renderer fixture golden mismatch** (`renderAndCompare`): writes the actual render as
  `<flattened-golden-name>.png` (NO `actual_` prefix) and `diff_<flattened-golden-name>.png`.
  There is no `expected_*.png` — the expected image is the committed golden itself (the `Expected:`
  line in the output points at it). These land in `$TEST_TMPDIR` (Bazel's per-test scratch dir),
  which is **not** collected into outputs.zip — copy the exact paths from the failure message's
  `Actual rendering:` / `Diff:` lines, or rerun via `bazel run` so they land in the system temp
  dir.
- **Renderer fixture identity checks**: `ExpectBitmapsIdentical`'s
  `actual_/expected_/diff_<label>.png` triple goes to `$TEST_UNDECLARED_OUTPUTS_DIR` →
  outputs.zip, like the editor helper. (The retired `GeodeTinyParity` mode's
  `parity_geode_* / parity_tiny_* / parity_diff_*` dumps used the same location.)

To isolate just-written files from temp-dir clutter, touch a marker before the run:

```sh
touch /tmp/marker && bazel test <target> --test_output=errors
find -L bazel-testlogs -name outputs.zip -newer /tmp/marker   # editor / parity outputs
# For renderer golden failures via `bazel run` (PNGs go to the system temp dir):
touch /tmp/marker && bazel run <target> -- --gtest_filter=Suite.Test
find "${TMPDIR:-/tmp}" -maxdepth 1 -name '*.png' -newer /tmp/marker
```

Reading the diff PNG: solid filled blocks = wrong pixel values (color, compositing, missing
element); paired outlines / double edges = positional drift (wrong transform); a diff that fills
the whole canvas = wrong canvas size, premultiplication, or color-space handling.

Renderer tests also print a truecolor terminal preview grid (actual / expected / diff) on failure.
When the environment variable `LLM=1` is set, that verbose output is suppressed; re-enable it with
`DONNER_RENDERER_TEST_VERBOSE=1 bazel test <target> --test_output=errors`.

## Anti-aliasing is NEVER the root cause

Restated from CLAUDE.md §"Anti-Aliasing Is Never the Root Cause" because pixel-diff triage is where
the temptation lives:

- pixelmatch runs anti-aliasing detection **by default** (our comparators set `includeAA = false`
  unless the params say otherwise), so anti-aliased edge pixels are already excluded before the
  mismatch count is reported. "It's just AA" doesn't merely lack proof — it contradicts the tool.
- Magnitude is the tell: genuine edge effects are a 1-pixel-wide band hugging boundaries, with
  single-digit to low-tens counts. Hundreds of pixels, per-glyph drift, whole-shape offsets, or
  block-shaped diffs are a real bug: wrong transform, wrong coverage geometry, wrong color space,
  wrong premultiplication, wrong layer compositing.
- If you truly believe a diff is edge AA, prove it: show it is a ≤1px edge band and quantify the
  per-channel magnitude (see the measured justifications in
  `donner/svg/renderer/tests/RendererGeodeGolden_tests.cc` for the required rigor). Absent that
  proof, the investigation is not finished.

## Golden test map and regeneration

| Suite                       | Test target                                               | Goldens                                                                              |
| --------------------------- | --------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| CPU renderer goldens        | `//donner/svg/renderer/tests:renderer_tests`              | `donner/svg/renderer/testdata/golden/`                                               |
| Renderer regression goldens | `//donner/svg/renderer/tests:renderer_regression_tests`   | shared `testdata/golden/`; separate binary — `renderer_tests` won't regenerate these |
| Geode renderer goldens      | `//donner/svg/renderer/tests:renderer_geode_golden_tests` | mostly shared `testdata/golden/`; a few under `testdata/golden/geode/`               |
| Editor replay goldens       | `//donner/editor/tests:rnr_replay_tests`                  | `donner/editor/tests/testdata/*.png`                                                 |
| Layer thumbnails            | `//donner/editor/tests:layer_thumbnail_golden_tests`      | `donner/editor/tests/testdata/layer_thumbnails/`                                     |

Regeneration always uses the `UPDATE_GOLDEN_IMAGES_DIR` environment variable pointing at the
workspace root, with `bazel run` (not `bazel test`, which sandboxes the writes):

```sh
# CPU renderer goldens — run with the DEFAULT backend (tiny-skia). Do NOT add --config=geode.
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
  bazel run //donner/svg/renderer/tests:renderer_tests

# Renderer regression goldens (separate binary, same golden directory)
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
  bazel run //donner/svg/renderer/tests:renderer_regression_tests

# Geode-only goldens (testdata/golden/geode/). The target is a transition wrapper that flips the
# backend itself, so this also works without --config=geode.
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
  bazel run --config=geode //donner/svg/renderer/tests:renderer_geode_golden_tests

# Editor replay goldens
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
  bazel run //donner/editor/tests:rnr_replay_tests

# Layer thumbnail goldens
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
  bazel run //donner/editor/tests:layer_thumbnail_golden_tests
```

Narrow the blast radius with `-- --gtest_filter=Suite.TestName` after the target. `.bazelrc`
already sets `run --test_sharding_strategy=disabled`, so `bazel run` works on sharded targets like
`rnr_replay_tests` (`shard_count = 5`). Filtering only narrows suites that are one-test-per-golden
(`renderer_tests`, `rnr_replay_tests`); `layer_thumbnail_golden_tests` is a single `TEST_F` that
loops over every layer, so a filter still regenerates all thumbnail goldens — scope the review via
its per-layer `[Name] wrote golden:` log lines and `git status` instead.

### Golden-sharing policy (renderer)

Renderer goldens are **shared across backends** — tiny-skia and Geode compare against the same
PNG, so the backends can never quietly diverge on geometry. The files under
`testdata/golden/geode/` are per-backend exceptions that are explicitly treated as coverage-gap
TODOs, not a design stance; see the file comment at the top of
`donner/svg/renderer/tests/RendererGeodeGolden_tests.cc`. That comment's category list (gradients,
patterns, image data-URL cases) is illustrative, not exhaustive — the directory also holds e.g.
`filters_*` and `quadbezier1` entries; treat any file there as a divergence to investigate, not a
mistake to delete on sight. A per-test Geode golden is
added via `ImageComparisonParams::withGeodeGoldenOverride(filename, reason)` and requires the
Geode output to be verified correct with a documented sub-pixel justification. Query the current
exception list with `ls donner/svg/renderer/testdata/golden/geode/`.

### Regeneration traps

1. **Update mode writes the golden and returns WITHOUT comparing.** With
   `UPDATE_GOLDEN_IMAGES_DIR` set, every _golden_ comparison writes the golden and passes without
   comparing (bitmap-to-bitmap comparisons like `CompareBitmapToBitmap` still run). Unset it (or
   use a fresh shell) before the verification run, or you will "verify" nothing.
2. **Regeneration blesses whatever the current build renders — including the bug you're chasing.**
   After regenerating, run `git status` and visually open every changed PNG (and its diff against
   the old version) before committing. A golden change without an explained pixel delta in the
   commit message is an unreviewed behavior change.
3. **Backend cross-contamination.** `renderer_tests` in update mode writes the _active backend's_
   pixels into the shared goldens. Running it under `--config=geode` (or regenerating shared
   goldens from the geode golden target) overwrites CPU-reference goldens with GPU output. Shared
   goldens are regenerated from the default tiny-skia build only; after any geode-target
   regeneration, inspect `git status` and revert changes to shared goldens you did not intend.
4. **Missing `data` dep.** A newly added golden PNG must be covered by the test target's
   `data` glob or explicit entry, or CI fails with a load error even though the file exists
   locally. `layer_thumbnail_golden_tests` and the renderer suites use real globs that pick up new
   PNGs automatically, but `rnr_replay_tests` uses a hand-maintained `rnr_replay_testdata`
   filegroup — a new golden there must be added to its `srcs` list in
   `donner/editor/tests/BUILD.bazel`.

## Authoring checklist for a new pixel-diff test

1. **Red before green.** The test must fail on the broken code before your fix, with the failure
   output (diff PNG, mismatch count) matching the reported symptom. See the donner-bugfix-discipline
   skill; commits claiming `Fixes #NNN` need that red→green evidence.
2. **Use the blessed helper** for the layer you are in (table above). Never inline pixelmatch or a
   loop over pixels in the test file.
3. **Start from `PixelmatchIdentityParams()`** (editor) or explicit strict params (renderer). In
   the renderer suite, beware that the file-local `compareWithGolden(...)` wrapper in
   `Renderer_tests.cc` defaults to `WithThreshold(0.1f)` — 5x looser than the raw
   `ImageComparisonParams{}` struct default (0.02/100px) — so copying the nearest example does NOT
   give you strict params. Pass
   `ImageComparisonParams::WithThreshold(0.0f, 0).includeAntiAliasingDifferences()` explicitly if
   identity is wanted. Only loosen via the approved-exception builders, with a comment naming the
   root cause of the slack.
4. **Render tightly around the suspect element** where the API allows (small canvas, single
   element subtree) so the diff count localizes the bug instead of drowning it in background.
5. **For spot checks of a few pixels**, use gmock matchers, not `EXPECT_EQ` per channel: the
   `Rgba(rMatcher, gMatcher, bMatcher, aMatcher)` / `RgbaEq` / `Near(expected, tol)` / `Alpha`
   matcher family in `donner/svg/renderer/tests/RendererGeode_tests.cc` (and `RgbaNear` in
   `donner/editor/tests/RenderElementToBitmap_tests.cc`) prints all four channels and names the
   failing one — copy that pattern. Rationale: a failing test must localize the bug without a
   rerun.
6. **Reference examples:** `donner/editor/tests/RnrReplay_tests.cc` (replay → `.rnr` →
   `CompareBitmapToGolden` / `CompareBitmapToBitmap` against a live ground-truth render),
   `donner/editor/tests/LayerThumbnailGolden_tests.cc` (identity-params golden per thumbnail),
   `donner/svg/renderer/tests/RendererGeodeGolden_tests.cc` (per-backend golden policy).

## Related

- **donner-bugfix-discipline** — red→green requirements, `attempt:` commit convention.
- **donner-editor-debugging** — `.rnr` replay capture, `bazel run --config=geode //donner/editor/tests:editor_rnr_gl_replay -- ... --crop document-canvas`, failure signatures;
  depth in `docs/editor_visual_debugging.md`.
- **donner-resvg-triage** — the resvg conformance suite, which uses the same
  `ImageComparisonTestFixture` and per-test override maps.
- **donner-geode-backend** — Geode backend internals when a GeodeGolden diff points at shaders or
  path encoding.
- **donner-build-test** — bazel test/run basics, variant lanes, `--config=geode`.
