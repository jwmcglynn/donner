# Design: Pivot tiny-skia-cpp backend to premul-internal storage

**Status:** Revived, implemented on a branch, pending accuracy adjudication
**Author:** Claude Opus 4.7 (original), Claude Opus 5 (revival)
**Created:** 2026-04-19
**Rejected:** 2026-04-19 (same-day, during implementation)
**Revived:** 2026-08-16

## Verdict

The 2026-04 rejection stands on its facts but drew the wrong conclusion from
them. Storing `RendererTinySkia::frame_` as premultiplied RGBA8 does lose
precision at low alpha, exactly as recorded below, and the three renderer
goldens it named still fail today for exactly that reason. What the original
verdict did not have was the size of the prize on the other side of the
tradeoff: a measured 1.9x median settled-frame speedup on the CPU backend. That
number changes the question from "is this lossy?" (yes, provably) to "is the
loss worth 1.9x?", which is a product call, not a correctness call.

The change is implemented and measured. The remaining decision is whether to
accept a bounded, characterized accuracy regression on antialiased edges. This
document records the measurements so that decision can be made from evidence.

## The change, concretely

1. Delete all 15 `paint.unpremulStore = ...` assignments in
   `RendererTinySkia.cc`. `tiny_skia::Paint::unpremulStore` defaults to false,
   so the root frame buffer joins every other surface the backend owns in
   holding premultiplied RGBA8, matching upstream tiny-skia.
2. `takeSnapshot()` runs `UnpremultiplyRgbaInPlace` once over the frame. The
   published `RendererBitmap` contract stays `AlphaType::Unpremultiplied`, so no
   consumer changes.

The storage decision now lives in one place (a note on the `frame_` member
declaration) instead of being re-derived at 15 call sites.

`unpremulStore` becomes entirely unused from Donner after this change. It is a
tiny-skia-cpp extension, not upstream tiny-skia API; the vendored copy is left
untouched so the flag can be removed on its own schedule.

## What the experiment measured (2026-08 rerun)

Setup: `-c opt`, single machine, no other load. Benchmark is
`engine_compare_bench --backend=tiny-skia --iterations=15 --warmup=3`; the
reported figure is the median settled-frame time (`second_ms`), which excludes
parse and first-frame warmup.

| Scene | Size | Before (ms) | After (ms) | Speedup |
| --- | --- | --- | --- | --- |
| Ghostscript_Tiger | 900x900 | 40.54 | 21.81 | 1.86x |
| lion | 567x567 | 4.485 | 2.716 | 1.65x |
| z0rly_test6 | 500x300 | 1.524 | 0.513 | 2.97x |
| big_lightning_glow_no_filter_crop | 95x177 | 0.365 | 0.185 | 1.97x |

Median speedup 1.92x. The snapshot-side unpremultiply pass costs about 2 ms at
900x900 and is charged against the "after" column above.

Conformance: the reference SVG suite runs 1512 cases per variant. Four exceed
their per-case pixel budget after the change, in both the `max` and
`default_text` variants:

| Case | Budget | Before | After |
| --- | --- | --- | --- |
| shapes/circle/simple-case | 100 | 14 | 112 |
| painting/marker/marker-on-rounded-rect | 100 | 24 | 4395 |
| painting/marker/nested | 100 | 0 | 909 |
| painting/marker/target-with-subpaths-1 | 100 | 0 | 25453 |

Three renderer goldens with a zero-tolerance budget also fail, the same three
the 2026-04 run named. The claim that those failures no longer reproduce is
wrong; they reproduce exactly. All three are alpha-preserving RGB shifts at
antialiased edges:

| Golden | Pixels over budget | Pixels changed | Max alpha delta | Max visible error over white |
| --- | --- | --- | --- | --- |
| MinimalClosedCubic2x2 | 4 of 0 allowed | 10 | 0 | 0.85 / 255 |
| MinimalClosedCubic5x3 | 9 of 0 allowed | 16 | 0 | 0.79 / 255 |
| BigLightningGlowNoFilterCrop | 446 of 0 allowed | 496 | 0 | 1.91 / 255 |

The canonical case is `MinimalClosedCubic2x2` pixel (4,3): golden
(17,17,17,6), now (0,0,0,6). That is mechanism 1 in its purest form, and it
moves the composited-over-white result by 0.40/255.

No other target regresses. Compositor goldens, the dual-path verifier, and
every GPU-backend variant are unaffected.

## Two independent mechanisms, separated by experiment

The failures above have two different causes, and separating them is the most
useful thing this rerun produced.

**Mechanism 1: u8 premultiplied storage quantizes RGB at low alpha.** This is
the original 2026-04 finding and it is correct. Pipeline-float state
(r=0.0666, a=0.0235) stores as `(unnorm(0.0666), unnorm(0.0235))` = (17, 6)
under straight-alpha storage, and as `(unnorm(0.0666 * 0.0235), 6)` = (0, 6)
under premultiplied storage. The multiply happens before the round-to-u8, so
the RGB information is gone and no later unpremultiply recovers it. At alpha 6
a premultiplied u8 pixel can only represent 7 distinct RGB levels.

**Mechanism 2: the flag was also pinning the raster pipeline to float.**
`Stage::Unpremultiply` and `Stage::PremultiplyDestination` are implemented only
in the float (high-precision) pipeline, so any paint with `unpremulStore` set
forced every draw touching the root surface onto that pipeline. Without the
flag, the blitter is free to select the 8-bit fixed-point pipeline, whose
compose arithmetic drifts: an opaque layer pixel composited through
`Painter::drawPixmap` can land at alpha 250 instead of 255, with the
premultiplied RGB preserved. That is what the marker cases are seeing.

Attribution experiment: rebuild the branch with the pipeline selector pinned to
float and rerun the four cases.

| Case | Before | After | After, pipeline pinned to float |
| --- | --- | --- | --- |
| shapes/circle/simple-case | 14 | 112 | 112 |
| painting/marker/marker-on-rounded-rect | 24 | 4395 | 24 |
| painting/marker/nested | 0 | 909 | 0 |
| painting/marker/target-with-subpaths-1 | 0 | 25453 | 0 |

So the three marker regressions are entirely mechanism 2, and
`shapes/circle/simple-case` is entirely mechanism 1. The three zero-tolerance
renderer goldens are also entirely mechanism 1 (they fail identically with the
pipeline pinned).

Pinning the pipeline to float measured within 1% of the unpinned branch on
every benchmark scene (Tiger 21.87 vs 21.81 ms, lion 2.745 vs 2.716 ms,
z0rly_test6 0.545 vs 0.513 ms, big_lightning 0.186 vs 0.185 ms). **The entire
1.9x comes from dropping the two conversion stages, not from the 8-bit
pipeline.** Mechanism 2 is therefore accuracy loss bought for approximately
nothing.

## How large is the visible error?

Raw RGBA deltas overstate the difference because fully transparent pixels carry
arbitrary RGB. Composited over white, against the reference PNGs:

| Case | Before, mean abs error | After, mean abs error | After, max channel error |
| --- | --- | --- | --- |
| shapes/circle/simple-case | 0.268 | 0.268 | 1 |
| painting/marker/marker-on-rounded-rect | 0.036 | 0.207 | 10 |
| painting/marker/nested | 0.040 | 1.149 | 7 |
| painting/marker/target-with-subpaths-1 | 0.027 | 1.088 | 11 |

`shapes/circle/simple-case` is genuinely marginal: no pixel moves by more than
1/255 against the previous rendering, and the mean error against the reference
is unchanged to three decimals. It crosses the budget only because the budget
counts pixels over a 1% threshold, and a large number of edge pixels sit right
at that boundary.

The three marker cases are not marginal in the same sense. Each moves tens of
thousands of pixels by up to 7-11/255 and moves measurably away from the
reference. They are, however, entirely attributable to mechanism 2.

Mechanism 1 on its own never exceeded 2/255 of visible error on any case
measured here, across the reference suite and the renderer goldens.

## Options

1. **Take the change as implemented.** Accept four suite cases and three
   zero-tolerance goldens over budget. Requires re-blessing the three renderer
   goldens and raising four suite budgets, which the suite's whole purpose
   argues against for the three marker cases.
2. **Take the change and pin the root pipeline to float.** Keeps the full 1.9x
   (measured within 1%), keeps the three marker cases green, and reduces the
   accuracy cost to mechanism 1 alone: `shapes/circle/simple-case` at 112 vs a
   100 budget, plus the three zero-tolerance goldens. This needs a way to
   request the float pipeline for pixmap composites, which today hardcode it
   off inside `Painter::drawPixmap`. That is a change to the vendored
   tiny-skia-cpp, not to Donner.
3. **Reject again.** The accuracy argument from 2026-04 is unchanged and
   correct on its own terms. Costs 1.9x on the CPU backend.

Option 2 is the recommendation: it is the only one that buys the speedup
without paying for mechanism 2, and the residual mechanism 1 cost is bounded
at 2/255 of visible error: one suite case at 112 against a 100 budget, plus
three zero-tolerance goldens whose largest visible change is 1.91/255.

## Notes carried forward from the 2026-04 pass

The shared `PixelFormatUtils.{h,cc}` extraction (`PremultiplyRgba` /
`UnpremultiplyRgba{,InPlace}` and their row-strided variants) landed on its own
and remains a net win independent of this decision. `takeSnapshot()` reuses
`UnpremultiplyRgbaInPlace` rather than growing a private copy.

The 2026-04 pass also cited a compositor round-trip precision concern
(`DualPathGate_ExplicitPromoteAtIdentity`, 19,200 pixels drifting by up to 2
channels). It does not reproduce on the current tree; the compositor suites are
green with the change applied.

The snapshot-side unpremultiply is a scalar loop that already short-circuits
alpha 255 and alpha 0, so an "is the frame opaque?" pre-scan would add a full
read without removing work. Vectorizing `UnpremultiplyRgbaInPlace` would help
every caller and is the useful follow-up.

## Lessons

- A perf-refactor design that changes pixel format needs both halves of the
  tradeoff before it can be judged: the exact bytes before and after on a
  representative low-alpha pixel, and the measured speedup. The 2026-04 pass
  had the first and not the second, so it could only conclude "lossy", never
  "lossy and worth it" or "lossy and not worth it".
- A storage-format flag can be load-bearing for something other than storage
  format. `unpremulStore` was documented as a precision guarantee; it was also,
  silently, a pipeline selector. Half of the accuracy regression came from the
  undocumented half, and that half was free to give back.
- Separate mechanisms before adjudicating. "Four cases regressed" and "three of
  the four regressed for a reason that costs 1% to fix" lead to different
  decisions.
