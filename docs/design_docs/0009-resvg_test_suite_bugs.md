# resvg-test-suite: Custom Golden Overrides

**Status:** Reference (current as of 2026-05-15)

When Donner's rendering of a resvg-test-suite case is correct but the upstream
golden is *not* the right thing to compare against, the test pins
`Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-<name>.png")`
so it compares against a Donner-blessed golden instead. This doc is the catalog
of every such override and the reason it exists.

There are two distinct reasons an override is justified:

1. **resvg's golden is wrong per spec** — Donner renders to the spec, resvg
   doesn't. These are genuine upstream golden bugs.
2. **Blessed minor diff** — Donner and resvg both render acceptably, but there's
   a small, well-understood difference (sub-pixel char advance, anti-aliasing on
   a curved baseline, a font-backend difference) where pinning a Donner golden is
   cleaner than carrying a per-test pixel threshold.

Custom goldens live in `donner/svg/renderer/testdata/golden/resvg-<test-name>.png`,
named with the **new** (post-Great-Rename) test stem. The rationale for each is
the `.withReason("…")` argument chained onto the override in
[`resvg_test_suite.cc`](../../donner/svg/renderer/tests/resvg_test_suite.cc) — grep
the test name there to find it inline.

> Naming note: these used the old `a-*` / `e-*` convention before the suite
> upgrade ([0022](0022-resvg_test_suite_upgrade.md)). The four originally
> documented here mapped forward as: `a-filter-013`/`a-filter-015` →
> `drop-shadow-function-mm-values`/`drop-shadow-function-em-values` (now parked,
> see below); `e-marker-045`/`e-marker-051` (closed-shape / cusp marker
> direction) were superseded upstream and retired — the surviving marker
> override is `orient=auto-on-M-C-C-4`.

## Active overrides (34)

### resvg golden is wrong per spec

| Category | Test | Reason (`withReason`) |
|---|---|---|
| filters/feSpotLight | complex-transform | resvg bug: SpotLight Y |
| filters/feImage | svg | We render higher quality |
| filters/feSpecularLighting | with-fePointLight | resvg golden |
| painting/marker | orient=auto-on-M-C-C-4 | Pre-existing rendering diff (stroke/AA), not cusp-related |
| painting/marker | with-an-image-child | We (correctly) render the image child |
| text/font-size | named-value | Donner uses CSS Fonts Level 4 |
| text/font-size | named-value-without-a-parent | Donner uses CSS Fonts Level 4 |

`named-value{,-without-a-parent}` reflect CSS Fonts Level 4's revised handling of
named font sizes (`xx-small` … `xx-large`), which differs from resvg's older table.

### Backend-difference goldens (text)

These render correctly but differ from resvg because Donner's text stack (and the
simple-vs-`text-full` split) shapes/composes differently. The golden is whichever
Donner backend the test runs under.

| Category | Test | Reason |
|---|---|---|
| text/text | complex-graphemes-and-coordinates-list | Simple text can't compose combining marks |
| text/text | compound-emojis-and-coordinates-list | Emoji bitmap scaling differs from the golden |
| text/text | rotate-on-Arabic | Arabic shaping requires text-full |
| text/text | x-and-y-with-multiple-values-and-arabic-text | Arabic shaping; vertical-axis AA diff not the focus |

### Blessed minor diffs (textPath / text-decoration)

The `text/textPath` family carries the bulk of the overrides: sub-pixel character
advance and anti-aliasing differences along curved baselines, which differ between
the simple and `text-full` backends. All are `.withReason("Minor char")` unless
noted.

| Category | Test | Reason |
|---|---|---|
| text/textPath | dy-with-tiny-coordinates | AA + minor char advance diffs (text vs text-full) |
| text/textPath | m-L-Z-path | Minor char |
| text/textPath | mixed-children-1 | AA diffs |
| text/textPath | nested | Minor char |
| text/textPath | path-with-ClosePath | Minor char |
| text/textPath | simple-case | Minor char |
| text/textPath | startOffset=-100 | Minor char |
| text/textPath | startOffset=10percent | Minor char |
| text/textPath | startOffset=30 | Minor char |
| text/textPath | startOffset=5mm | Minor char |
| text/textPath | tspan-with-absolute-position | Minor char |
| text/textPath | tspan-with-relative-position | Minor char |
| text/textPath | two-paths | Minor char |
| text/textPath | very-long-text | AA diffs |
| text/textPath | with-baseline-shift | Minor char |
| text/textPath | with-coordinates-on-text | Minor char |
| text/textPath | with-coordinates-on-textPath | Minor char |
| text/textPath | with-rotate | Minor char |
| text/textPath | with-text-anchor | (no reason string) |
| text/textPath | with-transform-on-a-referenced-path | Minor char |
| text/textPath | with-transform-outside-a-referenced-path | Minor char |
| text/textPath | with-underline | Minor char |
| text/text-decoration | indirect | (no reason string) |

> Two entries (`textPath/with-text-anchor`, `text-decoration/indirect`) carry no
> `withReason`. They should either get one or migrate to a per-test threshold; the
> "Minor char" cluster is the precedent. Tracked in
> [0021](0021-resvg_feature_gaps.md).

## Parked goldens (2) — filters/filter-functions disabled

`resvg-drop-shadow-function-mm-values.png` and
`resvg-drop-shadow-function-em-values.png` are the migrated successors of the old
`a-filter-013` / `a-filter-015` drop-shadow blur-halo cases: Donner's three-pass
box-blur approximation (correct per CSS Filter Effects L1 §8 / SVG §15.9) produces
a different halo shape than resvg's blur, so a Donner golden is required.

They are currently **unreferenced** because the entire `filters/filter-functions`
`INSTANTIATE_TEST_SUITE_P` block is commented out — that category throws
`"Data corrupted"` parse errors on CI x86_64 but passes locally on aarch64 (root
cause unknown). **Keep both files**: re-enabling the block (tracked in
[0021](0021-resvg_feature_gaps.md)) will need them. Do not delete as "orphans".

## Removed goldens (cleanup, 2026-05-15)

Three goldens added by [#515](https://github.com/jwmcglynn/donner/pull/515) were
never referenced by any override and were removed as dead data (recoverable from
git history if a real override need surfaces):

- `resvg-em-on-the-root-element.png`
- `resvg-ex-on-the-root-element.png`
- `resvg-percent-value-without-a-parent.png`

The corresponding `text/font-size` tests pass against the upstream golden — only
`named-value{,-without-a-parent}` need an override. After this cleanup the
`golden/` dir holds exactly 36 `resvg-*.png`: 34 active + the 2 parked drop-shadow
goldens above.

## Maintenance

- Adding an override: place `resvg-<new-stem>.png` in
  `donner/svg/renderer/testdata/golden/`, add `Params::WithGoldenOverride(…)
  .withReason("…")` in `resvg_test_suite.cc`, and add a row here. The
  `golden/*.png` glob in
  [`testdata/BUILD.bazel`](../../donner/svg/renderer/testdata/BUILD.bazel) picks it
  up automatically.
- Per [project policy](../../CLAUDE.md), never overwrite a golden without explicit
  approval — these encode deliberate spec/quality judgments.
- When a `Skip`'d gap in [0021](0021-resvg_feature_gaps.md) is fixed, check whether
  it needs an override here too.
