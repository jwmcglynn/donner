# Design: Pivot tiny-skia-cpp backend to premul-internal storage

**Status:** Rejected — precision-loss at u8 premul storage
**Author:** Claude Opus 4.7
**Created:** 2026-04-19
**Rejected:** 2026-04-19 (same-day, during implementation)

## Verdict

**Abandoned.** Storing `RendererTinySkia::frame_` as u8 premultiplied RGBA
loses irrecoverable precision at low alpha values. A pixel with
(rgb=17, a=6) — `#111111` at 6/255 coverage, a routine antialiased edge
— premultiplies to `17 × 6 / 255 = 0.4` in float space, which stores to
u8 as **0**. After the u8 roundtrip, no unpremultiply math can recover
the original rgb=17; the information is gone.

Tiny-skia's `Paint::unpremulStore = true` flag exists specifically to
dodge this: the pipeline blends in premultiplied float space and only
unpremultiplies on final u8 store, preserving per-channel precision at
low alpha. Donner has relied on that guarantee (sometimes unknowingly)
across its entire golden corpus. Flipping `frame_` to premul fails at
least three existing renderer goldens (2×2 cubic paths) on antialiased
edge pixels — and those are the *simple* scenes. More complex
goldens with semi-transparent edges would fail more broadly.

Skia sidesteps the same problem by doing blends in `kRGBA_F16_SkColorType`
intermediate buffers when precision matters and only converting to u8
at final present. Tiny-skia-cpp has no f16 pixmap support — its
pipeline is float in registers, u8 in storage, and every store/load
across surface boundaries quantizes. That's the structural difference
that makes Skia's "premul internal + unpremul at snapshot" safe and
tiny-skia's same-pivot unsafe.

## What was validated, and then kept vs reverted

**Kept:**
- `donner/svg/renderer/PixelFormatUtils.{h,cc}` — shared
  `PremultiplyRgba` / `UnpremultiplyRgba{,InPlace}` extracted from
  `FilterGraphExecutor.cc` and `CompositorController.cc`. Pure
  consolidation; byte-identical math to the originals. Both renderer
  + compositor golden suites pass unchanged.
- `FilterGraphExecutor.h` now re-exports `PremultiplyRgba` from the
  shared header.
- `CompositorController`'s `UnpremultiplyPixels` private copy deleted;
  `BuildImageResource` calls `UnpremultiplyRgba` from the new header.

**Reverted:**
- All `unpremulStore = ...` deletions in `RendererTinySkia.cc` — the
  flag is load-bearing and stays.
- `takeSnapshot()` stays a memcpy of `frame_.data()` (no unpremul pass
  needed because `frame_` is already unpremul by construction).

## What the experiment measured

Procedure (before revert):
1. Deleted every `paint.unpremulStore = surfaceStack_.empty() / &destination == &frame_`
   site in `RendererTinySkia.cc` — 14 call sites; default-false
   (`unpremulStore=false`) means premul storage.
2. Added `UnpremultiplyRgbaInPlace(snapshot.pixels)` in `takeSnapshot()`.
3. Ran golden suites.

Failure surface:
- **`//donner/svg/renderer/tests:renderer_tests`**: 3 failures —
  `MinimalClosedCubic2x2`, `MinimalClosedCubic5x3`,
  `BigLightningGlowNoFilterCrop`. Byte-level diff of the 2×2 case
  showed 10 pixels out of 100 mis-rendered at the antialiased path
  edge. Pixel (4,3) expected (17,17,17,6), actual (0,0,0,6) — rgb
  zeroed. Other affected pixels drifted by 1-3 units of rgb at
  alphas in the range 6-72.
- **`//donner/svg/compositor:compositor_golden_tests`**: 1 dual-path
  failure — `DualPathGate_ExplicitPromoteAtIdentity` — with 19,200
  pixels drifting by max 2 channels. This was the Stage-2 symptom
  the design doc anticipated (compositor round-trip through
  `Unpremul → BuildImageResource → drawImage → PremultiplyRgba`),
  bounded to ≤2 channels so plausibly a Stage-2 followup could
  address it. But the renderer-level failure is the fundamental
  blocker, not this.

## Why the design's precision analysis was wrong

The design claimed:

> `frame_` becomes premul; a single unpremul pass runs inside
> `takeSnapshot()`. Every existing PNG golden must byte-match before and
> after — pure performance refactor with zero observable output change.

The flawed step: "every existing PNG golden must byte-match." That
holds only if the pre-pivot and post-pivot u8 representations of the
same pipeline-float state produce the same snapshot bytes. They don't.

Concretely: pipeline-float state (r=0.0666, a=0.0235) after unpremultiply
stage (pre-pivot path) stores as `(unnorm(0.0666)=17, unnorm(0.0235)=6)`.
The same float state without the unpremultiply stage (post-pivot path)
stores as `(unnorm(0.0666*0.0235)=0, unnorm(0.0235)=6)` — the premul
multiplication happens *before* the unnorm round-to-u8, so the tiny
value rounds to 0.

The float→u8 rounding is lossy and asymmetric. Doing it pre-unpremul
loses precision on RGB at low alpha. Doing it post-unpremul (the
`unpremulStore=true` path) preserves it. Both produce a valid
premultiplied-or-unpremultiplied pixel, but the former has already
thrown away the information needed to recover the straight-alpha rgb.

My audit (informed by TinySkiaBot's thorough investigation) missed
this because I focused on *which surfaces are already premul* (all
non-root surfaces — true) and *whether filter primitives expect premul
input* (yes — true). Both correct. Neither implied that `frame_`
could safely make the same switch, because none of those non-root
surfaces exit the pipeline via a u8 unpremul snapshot to the
outside world. They're intermediate; they compose onto `frame_` (or a
wider parent) and the blend math on that parent still goes through
tiny-skia's precision-preserving float pipeline.

## What this means for the 1-2 ms/frame premul round-trip

The cost remains real. `compositePixmapInto` on the segment →
`frame_` blit path still runs `unpremul=true` and pays the premul →
blend → unpremul round-trip. On splash drag frames, that's still
~1-2 ms across 6-8 segments. Lost ground.

Alternative paths forward, ordered by expected ROI:

1. **Design 0027 Milestone 5 (Premul-Skip for Simple Segments).**
   Bypass `compositePixmapInto` entirely when the segment composes as
   opacity=1 / blendMode=SourceOver / no mask / integer translation.
   Direct unpremul → unpremul memcpy (with SIMD alpha-test). Works
   because segments are *already* stored unpremul on both ends, so a
   straight memcpy of the overlapping pixels is bit-identical to what
   the current round-trip produces for the simple case. Pure CPU win,
   no precision change. Previously scoped at ~1-2 ms/frame on splash.
   **Recommended next step.**

2. **Tiny-skia-cpp upstream: add an f16 Pixmap type.** Would
   structurally match Skia's design and let `frame_` be premul-f16
   with unpremul-u8 snapshot. Large cross-library change; tiny-skia
   upstream is written in a particular style and adding a second
   pixel-format family touches the entire pipeline stage table. Not
   worth it for the 1-2 ms/frame budget alone; revisit if the f16
   case appears elsewhere (e.g., wide-gamut color support).

3. **SIMD the `compositePixmapInto` unpremul pipeline.** Attack the
   cost in-place inside tiny-skia's blitter. Would benefit every
   composite, not just simple segments. Requires changes to
   `PipelineBlitter.cpp`'s lowp / highp stage selection when
   `unpremulStore=true`. Technically feasible; schedule risk is high
   because it's library-internal correctness-sensitive code. Defer
   unless (1) underdelivers.

4. **Don't optimize.** Accept the 1-2 ms. Tight-bounds already
   landed; Geode will have entirely different perf characteristics;
   the editor steady state is GL-texture-cached. The cost shows up
   only on drag-frame invalidations. Plausible pragmatic choice if
   higher-leverage work dominates the roadmap.

## Lessons

- **Perf-refactor designs need a "reproduce byte-identical output on a
  representative test input" gate before any code lands.** The design
  claimed this as a property of the pivot, but the analysis for *why*
  it should hold was absent. Pushing the precision question earlier
  in the design phase — "what does tiny-skia's `unpremulStore=true`
  actually protect?" — would have caught this without writing code.
- **Storage-format decisions in low-precision pipelines are
  non-obvious.** "Skia does X so we can do X" ignored the
  intermediate-f16 architectural difference. For tiny-skia's u8-only
  pipeline, every surface boundary is a quantization point; reducing
  the number of boundaries (fewer unpremul passes) only helps if you
  don't introduce new lossy boundaries (premul stores at low alpha).
- **The extracted `PixelFormatUtils` helper is a net win regardless.**
  Three copies of `PremultiplyRgba` collapsed to one, and
  `CompositorController`'s private `UnpremultiplyPixels` is now a
  peer. Worth keeping as a separate, trivially-reviewable commit.

## What the design doc should have said up front

The doc's "Premultiplied RGBA8 loses precision at low alpha values."
line item was buried under "Non-Goals — precision loss: premul storage
(uint8) clips low-alpha channels to discrete steps." That's not a
non-goal, it's a *reason not to do the change*. Moving it to the
front — before "the change, concretely" — and verifying it against a
concrete low-alpha example would have ended the design at that step.

Future perf-refactor designs touching pixel format should lead with:

> "On our representative low-alpha test pixel, what bytes does the
> current code store? What bytes will the proposed code store?
> Demonstrate they are bit-identical, or describe the drift bound."

If that can't be answered at design time, the design is not ready.
