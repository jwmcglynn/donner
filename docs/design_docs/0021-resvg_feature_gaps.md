# resvg-test-suite: Feature Gaps & Open Bugs

**Status:** Living catalog (current as of 2026-05-15)

The triage backlog for [0022](0022-resvg_test_suite_upgrade.md)'s Milestone 2 —
working through the tests the suite upgrade pulled in and either fixing the
underlying gap or recording why a `Params::Skip(...)` is the correct state. Each
entry corresponds to one or more skips in
[`resvg_test_suite.cc`](../../donner/svg/renderer/tests/resvg_test_suite.cc).

**[Geode (GPU backend) parity leads this backlog](#geode-parity--priority-0)** —
reaching pixel parity with the CPU rasterizer is Priority 0; the CPU-backend
feature gaps follow.

When a gap is fixed, delete its entry here and un-skip the tests **in the same
PR**. Golden overrides (where Donner is right and resvg's golden is wrong) live in
[0009](0009-resvg_test_suite_bugs.md), not here.

**Conventions:**
- **Impact** = number of currently-skipped tests the entry covers.
- **Root cause** = best-known localized explanation, or "needs investigation".
- **Next step** = what a fix PR touches first.
- Prefix `B` = bug (Donner is wrong), `F` = feature gap (standard feature not
  implemented). Numbers are stable IDs other docs/PRs can reference; retired
  entries leave their number burned.

## Current totals

| | Count |
|---|---:|
| `Params::Skip(...)` | 288 |
| `Params::RenderOnly(...)` | 51 (mostly UB cases — render-must-not-crash, no pixel compare) |
| Commented-out category blocks | 1 (`filters/filter-functions`) |

## Geode parity — Priority 0

The GPU backend ([Geode](0017-geode_renderer.md)) must reach pixel parity with the
CPU rasterizer (RendererTinySkia) across the resvg suite. As of 2026-05-15 the
Geode variant (`:resvg_test_suite_geode`, wired at
[`BUILD.bazel:447`](../../donner/svg/renderer/tests/BUILD.bazel)) runs **1,345
pass / 0 fail / 291 skipped** of 1,636 cases. The 291 skips are **268 text** (the
whole text suite gated off) + **23 per-test pixel divergences**. Closing those is
the parity goal, and it leads this backlog.

The structural stubs are gone — `drawText`, `pushFilterLayer`, clip, mask, blend
modes, markers, patterns, gradients, and images are all implemented. Of 252 text
tests, 162 fail on Geode; that splits into **4× MSAA edge-coverage quantization**
(the bulk — accepted, Geode stays at 4× for perf) and **real structural bugs**
(G1-struct — `drawText` renders no text stroke/decoration, plus CJK/vertical/
rotate+pattern cases), alongside a **handful of filter-primitive bugs** (G2).

> Source of truth for the per-test divergences: the 8 `disableBackend(Geode, …)`
> gates in [`resvg_test_suite.cc`](../../donner/svg/renderer/tests/resvg_test_suite.cc)
> (`geodeCategoryGate` / `geodeFilenameGate`). Narrative + phase plan:
> [0017 Phase 5b/7](0017-geode_renderer.md). This section is the prioritization,
> not a duplicate list — don't fork the gate list here.

> ⚠️ **The "0 failing" number is softer than it looks.** Many Geode tests pass only
> because their pixel threshold was widened with an "AA drift" / "MSAA vs
> supersample" justification (~40 such sites across the Geode test code). Per
> [CLAUDE.md §"Anti-Aliasing Is Never the Root Cause"](../../CLAUDE.md), that
> reasoning is invalid: pixelmatch already excludes anti-aliased pixels, so a
> nonzero diff is a real difference, not AA. Each AA-justified Geode threshold is a
> **candidate masked bug** and needs re-auditing — passing-with-a-fat-threshold is
> not parity. This audit is part of the parity work (G2/G5 below), not separate.

### G1: Geode text parity — split into edge-sampling (accepted) + structural bugs

**Status:** investigated 2026-05-15 to ground truth (full geode text run, pixel
analysis, code read). Repro committed —
`GeodeTextDecorationRepro.UnderlineNotRenderedOnGeode` in
[`resvg_test_suite.cc`](../../donner/svg/renderer/tests/resvg_test_suite.cc)
(red on Geode, authored + green on both CPU text tiers).

**Methodology (corrected):** measure parity as geode-vs-**tiny-skia**, not
geode-vs-resvg-golden. tiny-skia passes **all 252** text tests with the suite's
per-test thresholds; geode fails **162** of the same tests with the same params —
so all 162 are **geode-specific**. (My earlier "most text passes" was a sampling
artifact; the full run is 88 pass / 162 fail.)

The 162 split into two very different buckets:

#### G1-edge: 4× MSAA edge-coverage quantization (the bulk) — accepted, not fixed

The majority of failures (median ~697 px) are **edge-coverage quantization**.
Proven on `text-anchor/end-on-text`: the text bbox is **pixel-identical** to the
reference (`x[21..246] y[164..250]`, same size, ink within 1%), and geode's edge
alpha is **quantized to 64-steps (0/64/128/191/255)** = exactly 4 MSAA samples,
vs the reference's smooth coverage. This is the one place "edge sampling" is the
*proven* cause (per [CLAUDE.md §"Anti-Aliasing Is Never the Root Cause"](../../CLAUDE.md),
proven by a 1px edge band + quantified — not assumed).

**Decision: Geode stays at 4× MSAA for performance.** So G1-edge is *not* a bug to
fix — when un-gating, these tests get a documented per-test threshold widening
attributable to the 4× sample count (a proven, deliberate tradeoff, not AA
hand-waving). Glyphs at integer positions already pass (`font-weight/650` → 3 px);
only fractional-position edges diverge.

#### G1-struct: real structural bugs (the tail) — these get fixed

Genuinely missing/broken rendering, far too large for edge coverage:

- **`drawText` decoration** — `drawText` rendered no text-decoration
  (underline/overline/line-through), while `RendererTinySkia::drawText` does.
  ✅ **Fixed** (decoration *fill*) — ported metrics → rect → `fillPath`; the repro
  is green and the existing geode golden suite is unaffected.
- **`drawText` text + decoration stroke** — `drawText` drew no `stroke="…"` on
  text. ✅ **Fixed** — re-enabled glyph + decoration stroke via
  `placed.strokeToFill(...)`. (Two wrong hypotheses on the way, both ruled out by
  measurement: it is *not* a "~2.5× too thick" width bug — element `strokeWidth=0`,
  the stroke is span-driven and 2.5px is the correct device scale. The real bug was
  the **fill rule**: closed contours expand to same-winding outer+inner subpaths,
  so the ring needs `EvenOdd` (`Impl::strokeFillRuleFor`), not `NonZero` which
  filled glyph interiors solid. Verified hollow ring; decoration tests 0 → 3.)
- ~~**Default-fill resolution for stroked text**~~ — recorded as a "missing black
  fill" bug (default-fill `<text stroke="…">` resolving `spanFill` alpha=0, glyphs
  rendering as the stroke ring only). **Investigated 2026-05-24: not a bug at HEAD**
  (`aab9665f6`). Instrumented `drawText` measures `spanFill=(0,0,0,255)`, `hasFill=1`
  across plain / nested (`all-types-nested.svg`) / tspan-child / inherited-from-group
  variants; a tiny-skia-authored golden diffs at ~201 px (the accepted G1-edge 4×
  MSAA floor) with black-filled glyphs + a hollow stroke ring confirmed visually. The
  original note was an in-flight artifact of the stroke fill-rule fix (same commit),
  recorded as a hypothesis without a committed red repro — there is no fix to make.
> ⚠️ **Metric caveat (added 2026-05-24):** the px figures below were measured
> **geode-vs-resvg-golden** — the wrong oracle (parity is geode-vs-**tiny-skia**, per
> the methodology note above). They are inflated by the shared ~1313 px
> tiny-skia-vs-resvg baseline offset (the documented ~2 px vertical-baseline
> reference gap, already covered by the suite's `withMaxPixelsDifferent(1500)` on the
> tb tests) plus the ~950 px sub-pixel crosshair/frame floor common to the whole
> writing-mode category. **Re-measure geode-vs-tiny-skia before treating any of these
> as a bug** — two of the three originally listed (default-fill, vertical) evaporated
> under the correct metric.

- ~~`text/writing-mode=tb` (vertical) → ~2.3k px~~ — **not a bug** (investigated
  2026-05-24). Geode-vs-tiny-skia is **9 px** (G1-edge floor) across all six vertical
  tests (`tb`, `tb-rl`, `tb-with-alignment`, `inheritance`, `vertical-lr`,
  `vertical-rl`); a bare vertical repro is 0 px. The 2.3k was geode-vs-resvg (~1313
  baseline + ~950 crosshair/frame). The vertical rotate-only transform is identical
  in both `drawText` paths.
- `text/text/xml_lang_ja` (CJK) → ~19k px **geode-vs-resvg — re-measure**. Likely
  real + structural anyway: bitmap-only/CBDT glyphs are skipped in `drawText`
  (`RendererGeode.cc:3162`) — overlaps [G4](#g4-color-emoji--bitmap-fonts-structural).
- `text/text/rotate…underline…pattern` → ~105k px **geode-vs-resvg — re-measure**.
  Magnitude is large enough to be a real pattern-on-rotated-text bug regardless.

**Latent (no failing test yet):** the two `drawText` paths compose the per-glyph
transform differently — tiny-skia does stretch-on-outline then `Rotate*Translate`
(`RendererTinySkia.cc:1588-1650`); Geode does `Scale*Rotate*Translate`
(`RendererGeode.cc:3216-3224`). These diverge only when `stretchScale≠1` **and**
`rotateDegrees≠0` at once — i.e. vertical text + `textLength`/`lengthAdjust=spacingAndGlyphs`.
No current test hits that combo (0 px contribution here); file a properly-scoped repro
before fixing.

**Plan:**
1. ✅ Repro committed (text-decoration underline, red on Geode).
2. ✅ **Decoration fill** rendered in `drawText` — repro green, no golden-suite
   regression.
3. ✅ **Text + decoration stroke** rendered with the correct fill rule.
4. ✅ **Default-fill resolution for stroked text** — investigated, *not a bug* at
   HEAD (see struck bullet above); the recorded symptom doesn't reproduce. No change.
5. Work the remaining G1-struct tail — **re-measure geode-vs-tiny-skia FIRST** (the
   doc's px figures are geode-vs-resvg; two of three "bugs" evaporated under the
   correct metric). Live candidates: CJK (`xml_lang_ja`, bitmap-glyph skip / G4) and
   rotate+pattern (~105k). Vertical is resolved (9 px floor). **← active.**
6. **Un-gate** via an **additive in-process backend matrix** — full plan in
   [0017 §Phase 4b](0017-geode_renderer.md#phase-4b-in-process-backend-matrix--geode-vs-tiny-skia-parity-comparison).
   One geode-enabled binary runs three comparison modes per test (`TinyGolden`,
   `GeodeGolden`, and `GeodeTinyParity` which ignores the golden). **Landed green
   (2026-05-24)** at the final policy: parity = pixelmatch `includeAA=false`, per-pixel
   `kDefaultThreshold` (0.02), **flat 100-px max-count, no per-test thresholds** (not the
   rejected `widenThresholdForGeode` 0.3 bump, which masks solid-region bugs — the
   [G5](#g5-audit-the-aa-justified-geode-thresholds) pattern). Tests >100 px are **binary-
   gated** via `geodeParityGate`, organized as an inventory: **172 edge-floor** (4× MSAA
   edge quantization, 101–763 px — one shared reason, ratchet out together with finer AA) +
   **56 genuine** (**38 filter** → reference [G2](#g2-filter-primitive-correctness-16-of-23-disabled-tests);
   **18 text/text-on-shape** → reference [0038](0038-geode_tinyskia_text_parity.md)).
   Whole-suite parity: **1035 pass / 228 gated** (text 86/159, non-text 949/69). The ~137
   sub-visual premultiply fills pass at 0.02 → [G5](#g5-audit-the-aa-justified-geode-thresholds).

   **Progress since landing (2026-05-24, see [0038](0038-geode_tinyskia_text_parity.md)):** the
   text hoist resolved **all structural text divergences** (`kGenuineText` empty — gradient/
   pattern-on-text + a shared-layout baseline-shift idempotency bug fixed, rest render-correct
   edge-floor), and G2 fixes: a **linearRGB color-interpolation-filters** sweep for feComposite,
   feComponentTransfer + feTurbulence + feMerge + feConvolveMatrix + feDiffuse/SpecularLighting
   (geode was applying it to some primitives but not others), an **feImage shared-`RendererDriver`
   re-draw idempotency fix** (8 tests), a **feDisplacementMap** correctness fix (un-premult +
   bilinear + linearRGB), and a **feSpecularLighting spec fix** (specularExponent [1,128] clamp +
   `<1`→transparent), and **`feGaussianBlur/complex-transform`** (anisotropic-blur-under-rotation —
   ported tiny's transformed-blur path into geode's `popFilterLayer`: resample device→local, blur
   in local space, composite back; 35151→140px). **All 37 original filter divergences resolved —
   G2 is fully closed.** **Gate ledger now: 0 text + 0 G2 + 192 edge-floor = 192 gated** (down from
   228); the remaining 192 are exclusively the **accepted-by-design** geode-vs-tiny AAA-coverage /
   crosshair sub-pixel floor (content matches, unfixable in-renderer — [0039 §2](0039-geode_analytical_aa.md)). Two real
   production idempotency bugs (baseline-shift, feImage) surfaced by the parity harness's
   double-draw, plus a systematic geode filter linearRGB gap + spec-conformance bugs + the
   transformed-blur path — all fixed in shared/geode code, not backend quirks. **Text + filter
   parity complete.**

### G2: Filter-primitive correctness (16 of 23 disabled tests)

Concentrated, well-scoped shader/kernel bugs (file:line into
[`resvg_test_suite.cc`](../../donner/svg/renderer/tests/resvg_test_suite.cc)):

- **feConvolveMatrix** edge-mode / targetX / preserveAlpha / order=4 — 9 tests (`:206`)
- **feImage** subregion / `preserveAspectRatio` placement, diverges between Mesa
  llvmpipe and lavapipe (needs driver-independent placement) — 5 tests (`:364`)
- **genuine pixel regressions**: feMorphology w/ opacity, feSpecularLighting
  exponent=0, feTile empty-region, filter transform-on-shape — 4 tests (`:228`)
- **feComponentTransfer** gradient + mixed per-channel types — 1 (`:332`)
- **feDiffuseLighting** no-light-source draws black instead of no-op pass-through — 1 (`:346`)

### G3: Driver / perf one-offs (low priority)

- **feMorphology huge-radius** > 30 s on Mesa llvmpipe trips the per-testcase
  watchdog — 1 test (`:406`); kernel tuning, ties into
  [0030](0030-geode_performance.md).
- **marker `orient=auto`** tangent disagrees at curve cusps — 1 test (`:251`); real
  geometry bug.

### G4: Color-emoji / bitmap fonts (structural)

CBDT/CBLC bitmap glyphs are skipped inside `drawText`
([`RendererGeode.cc:3162`](../../donner/svg/renderer/RendererGeode.cc)) — they need
the `GeodeTextureEncoder` path wired up. Smaller scope; sequence after G1 lands the
outline-text suite.

### G5: Audit the AA-justified Geode thresholds

**Impact:** ~40 widened-threshold sites across the Geode test code (grep
`resvg_test_suite.cc`, `Renderer_tests.cc`, `RendererGeodeGolden_tests.cc`,
`RendererTestBackendGeode.cc` for `MSAA` / `supersample` / `AA drift`).

**Symptom:** these tests "pass" only with an inflated per-pixel threshold whose
comment blames anti-aliasing. pixelmatch already excludes AA pixels, so the
absorbed diff is real — each is a possible Geode rendering bug hiding behind the
threshold (the #582 pattern).

**Next step:** for each, drop the threshold to default, capture the real diff
(`actual/expected/diff` PNGs), and root-cause it. Replace the AA comment with the
true cause or a tracking link. Don't reword the comments without doing the
investigation — that just relabels the masking.

**G5-premultiply (quantified 2026-05-24, the Phase 4b parity run):** ~137 non-text
tests show a **uniform, sub-perceptual color/alpha offset across solid fills** — the
*whole* fill region differs from tiny-skia at strict-0, but the diff collapses to
<100 px at the suite's default 0.02 threshold (and is invisible to the eye). The
likely single root cause is **premultiplied-alpha / color-space rounding** in Geode's
solid + filter output path (e.g. unpremultiply-on-store rounding vs tiny-skia). These
**pass** the Phase 4b parity gate at 0.02 (not gated), but they are exactly the G5
"sub-threshold real diff" pattern at scale: a 1–2 level offset on a 400×400 fill =
160000 px at threshold 0. **One premultiply/rounding fix likely clears most of them.**
Repro: run the `GeodeTinyParity` mode at threshold 0 (vs 0.02) and diff a solid-fill
test such as `filters/feBlend/mode=multiply` or `painting/fill/rgb-0-127-0-0.5` — the
diff is the entire solid interior, uniformly. Tracked as the durable color-output item.

---

## Backlog ranked by leverage (CPU backends)

Geode parity (above) is Priority 0. The rest of this doc is the CPU-backend
(RendererTinySkia / Skia) feature gaps. Highest-value first; "out of scope" rows
are correct-as-skipped and listed at the bottom for completeness.

| ID | Gap | Impact | Kind |
|---|---|---:|---|
| F2 | `transform-origin` rendering regression (#514) | 21 | Regression — single PR |
| B2 | `filters/filter-functions` disabled (CI "Data corrupted") | ~30 | CI gap — whole category dark |
| B3 | `<image>` embedded/data-URL sizing | 18 | Bug — one investigation |
| F3 | `context-fill` / `context-stroke` | 13 | Feature |
| F4 | `<switch>` conditional processing | 12 | Feature |
| F5 | full `dominant-baseline` keyword set | 14 | Feature |
| F6 | full `alignment-baseline` keyword set | 10 | Feature |
| B1 | intrinsic sizing + percent on non-square viewBox | ~10 | Bug — one root cause, many categories |
| F7 | `paint-order` rendering | 9 | Feature (parsed, not rendered) |
| B4 | `<use>` → inline `<svg>` sizing | 5 | Bug |
| B5 | `feMorphology` edge cases | 5 | Bug |
| F8 | primitive subregion clipping (feBlend/feComposite/feFlood) | 5 | Feature |
| F9 | `textLength` + `lengthAdjust` stretch/compress | 8 | Feature |
| F10 | `textPath` SVG2 attributes (`path`/`side`/`method`/`spacing`) | 8 | Feature |
| F11 | BiDi / RTL text shaping | ~8 | Feature |
| — | masking edge cases (mask 8, clipPath 11) | ~19 | Mixed |
| — | uncertain `Bug?` entries (need triage) | ~12 | Needs investigation |
| F1 | `enable-background` + `in=Background*` | 23 | **Out of scope** (deprecated) |
| — | other deprecated/UB skips | ~30 | **Out of scope** |

---

## Tracked regressions & disabled blocks

### F2: `transform-origin` rendering broken after #514

**Impact:** 21 tests — all of `structure/transform-origin/`.

**Symptom:** 10K–150K-pixel diffs — rendering is completely wrong, not slightly
off. Tagged `Skip("transform-origin broken after #514")` with `TODO(#514)` at
[`resvg_test_suite.cc:1593`](../../donner/svg/renderer/tests/resvg_test_suite.cc).

**Root cause:** PR #514 added single-keyword `transform-origin` parsing (e.g.
`transform-origin: center`) and regressed the rendering path in the process.
Before #514 the two-token forms applied correctly; after #514 the transform
pivots around the wrong origin.

**Next step:** bisect #514's diff. The parser change and the apply-at-render change
are separable — verify `ParseTransformOrigin` output, then check
`LayoutSystem::createComputedLocalTransformComponentWithStyle` consumes the
resolved origin. Land the fix and un-skip all 21 in one PR. **Highest single-PR
payoff in the backlog** (it's a regression, so there's a known-good prior state).

> Note: this entry previously described `transform-origin` as a never-implemented
> *presentation attribute*. That gap was closed; the current failure is the #514
> *regression* above. The presentation-attribute form should be re-verified once
> the regression is fixed.

### B2: `filters/filter-functions` category disabled on CI

**Impact:** ~30 tests — the entire `filters/filter-functions/` block.

**Symptom:** The `INSTANTIATE_TEST_SUITE_P(FiltersFilterFunctions, …)` block is
commented out ([`resvg_test_suite.cc:872`](../../donner/svg/renderer/tests/resvg_test_suite.cc)).
The category produces `"Data corrupted"` parse errors on CI x86_64 runners but
passes locally on aarch64.

**Root cause:** unknown. Candidates: a resvg-test-suite data-integrity issue on
CI, an x86_64-specific parser bug, or a runfiles/encoding difference between the
runners. This is exactly the CI-vs-local gap the project's always-green-main
policy calls out — the fix is to close the gap, not route around it.

**Next step:** reproduce on an x86_64 runner (or container). Capture the exact SVG
that triggers `"Data corrupted"` and minimize it. These tests were enabled once in
[#515](https://github.com/jwmcglynn/donner/pull/515) before being disabled, so the
rendering path works — this is an input/parse problem on one arch. Two custom
goldens (`drop-shadow-function-{mm,em}-values`) are parked for re-enable; see
[0009](0009-resvg_test_suite_bugs.md).

---

## High-leverage bugs (one root cause, many tests)

### B1: Intrinsic sizing + percent resolution on non-square viewBox

**Impact:** ~10 tests across 7 categories (all carry the reason string
`"… intrinsic sizing + percent resolution with non-square viewBox; see
ShapesEllipse"`).

**Symptom:** For SVGs with a non-square `viewBox` (e.g. `0 0 200 100`) and no
explicit `width`/`height`, the intrinsic document size is wrong. Under the
fixture's `setCanvasSize(500, 500)`, Donner produces 500×375 instead of 500×250,
so percent-valued geometry (`cx="50%"`, `rx="40%"`, …) resolves against the wrong
viewport and renders oversized / off-center.

**Root cause:** `LayoutSystem::calculateRawDocumentSize`
([LayoutSystem.cc:806](../../donner/svg/components/layout/LayoutSystem.cc)) uses
`transformPosition()` on the viewBox→content transform, which folds in the
letterbox translation. `transformVector()` (no translation) is the correct call.
The percent-resolution pipeline is coupled to this, so fix both together.

**Next step:** swap `transformPosition`→`transformVector`, audit percent
resolution in `ComputedShapeComponent`/`LayoutSystem`, land as one PR.

**Affected tests:** `shapes/ellipse/percent-values{,-missing-ry}`,
`shapes/line/percent-units`, `shapes/rect/percentage-values-{1,2}`,
`paint-servers/linearGradient/gradientUnits=userSpaceOnUse-with-percent`,
`paint-servers/radialGradient/gradientUnits=objectBoundingBox-with-percent`,
`painting/marker/percent-values`,
`text/text/percent-value-on-{x-and-y,dx-and-dy}`.

### B3: `<image>` embedded / data-URL sizing

**Impact:** 18 tests in `structure/image/` (plus 2 external-URL `Not impl`, 4 UB
RenderOnly).

**Symptom:** Embedded images (data URLs, embedded PNG/JPEG/GIF/16-bit/SVG) render
but at the wrong size; `preserveAspectRatio` modes
(`none`/`xMin/Mid/Max…-meet`/`slice`) and the `no-width`/`no-height`/`auto` sizing
cases disagree with the golden.

**Root cause:** needs investigation — `<image>` layout/sizing and
`preserveAspectRatio` resolution for raster + nested-SVG content.

**Next step:** start with `embedded-png` + `preserveAspectRatio=none` (the
simplest), then walk the no-width/no-height/auto matrix.

### B4: `<use>` referencing inline `<svg>` elements

**Impact:** 5 tests in `structure/use/`.

**Symptom:** `<use>` of an inline `<svg>` with various `width`/`height`/`viewBox`
combinations sizes the instance wrong.

**Next step:** likely shares machinery with B3's viewport sizing; investigate
together if convenient.

### B5: `feMorphology` edge cases

**Impact:** 5 tests in `filters/feMorphology/` (`empty-radius`, `negative-radius`,
`no-radius`, `radius-with-too-many-values`, `zero-radius`).

**Symptom:** degenerate/invalid radius values aren't handled to spec.

**Next step:** add radius validation/clamping in the feMorphology primitive per
Filter Effects §15.

---

## Unimplemented features (clean single-feature scope)

### F3: `context-fill` / `context-stroke`

**Impact:** 13 tests in `painting/context/`. Parsed but not honored at render.
Used by markers and `<use>` to inherit the referencing element's paint.

### F4: `<switch>` conditional processing

**Impact:** 12 tests in `structure/switch/` (+1 in `clipPath`). Includes
`requiredFeatures` / `systemLanguage` evaluation. Related: `structure/systemLanguage`
(3), which F4 should subsume.

### F5: full `dominant-baseline` keyword set

**Impact:** 14 tests in `text/dominant-baseline/`. Missing `before-edge`,
`after-edge`, `no-change`, `reset-size`, `use-script`, etc.

### F6: full `alignment-baseline` keyword set

**Impact:** 10 tests in `text/alignment-baseline/`. Full keyword set + tspan
baseline alignment.

### F7: `paint-order` rendering

**Impact:** 9 tests in `painting/paint-order/`. The property name parses but
render order (fill/stroke/markers) is not reordered. On shapes, text, and tspan.

### F8: primitive subregion clipping

**Impact:** 5 tests (`filters/feBlend` 2, `filters/feComposite` 3 incl. feFlood
subregion). Filter primitives don't clip output to their `x`/`y`/`width`/`height`
subregion.

### F9: `textLength` + `lengthAdjust`

**Impact:** ~8 (`text/textLength` 4 + `text/lengthAdjust` 3 + `text/text-decoration`
interaction). Text stretching/compressing to a target length (`spacing` and
`spacingAndGlyphs`), including the Arabic cases.

### F10: `textPath` SVG2 attributes

**Impact:** 8 in `text/textPath/`: `path` attribute, `side=right`, `method=stretch`,
`spacing=auto`, `path`+`xlink:href` combinations, `filter` on textPath, plus the
deferred vertical/`writing-mode=tb` cases.

### F11: BiDi / RTL text shaping

**Impact:** ~8 across `text/direction` (2), `text/unicode-bidi` (1),
`text/text/bidi-reordering`, `text/tspan/bidi-reordering`,
`text/letter-spacing/mixed-scripts`, `text/textLength` Arabic. Needs the BiDi
algorithm + RTL shaping (`text-full`). Group as one workstream.

### Smaller feature gaps

| Category | Tests | Gap |
|---|---:|---|
| structure/a | 3 | `<a>` hyperlink rendering |
| structure/svg | 2 | nested-svg `overflow` |
| structure/style | 1 | CSS `@import` / external CSS |
| structure/symbol | 1 | `transform` on `<symbol>` (SVG2) |
| painting/image-rendering | 2 | `image-rendering` (pixelated/crisp-edges) |
| masking/clipPath | 6 | clipPath with `<text>` children, `<use>` child, shorthand edge cases |
| masking/mask | 8 | `mask-type`, `mask-units`, `color-interpolation`, mask-on-self |
| text/font | 2 | `font` shorthand; canvas-size mismatch (test harness) |
| text/tspan | 3 | tspan interaction with `clip-path`/`filter`/`mask` |
| painting/stroke-dasharray | 4 | `0 n` dash patterns with caps |
| painting/marker | 4 | multiple closepaths, rounded-rect corners, recursive-5 |
| text/writing-mode | ~6 | `writing-mode=tb` with `dx`/`dy`, vertical-lr/rl edge cases |

---

## Needs triage (uncertain `Bug?` entries)

These have a question-mark reason in the file and need a root-cause pass to decide
bug vs. out-of-scope:

- `structure/svg`: XML Entity references (3), mixed namespaces, non-UTF-8 encoding,
  rect-inside-non-SVG-element, xmlns validation
- `paint-servers/stop`: `stop-color` inherit edge case
- `text/letter-spacing/non-ASCII-character`: different CJK glyph (wrong font?)
- `text/textLength/on-text-and-tspan`: we compress more than the golden
- `text/font-family/fallback-1`: fallback from invalid family
- `masking/clip/simple-case`: empty `Skip()` with no reason — must get a reason or
  be fixed

---

## Out of scope (correctly skipped — do not "fix")

| Category | Tests | Why |
|---|---:|---|
| filters/enable-background | 21 | `enable-background` deprecated in SVG 2 (→ `<filter>` chains / `backdrop-filter`). See [`unsupported_svg1_features.md`](../unsupported_svg1_features.md). |
| filters/filter `in=Background*` | 2 | Same deprecation (BackgroundImage/BackgroundAlpha inputs). |
| text/tref | 9 (+1 display) | `<tref>` removed in SVG 2. |
| text/kerning | 2 | `kerning` attribute deprecated SVG 1.1. |
| text/glyph-orientation-* | 2 | deprecated SVG 1.1. |
| paint-servers/radialGradient | 2 | test-suite bugs (`focal-point-correction`, `fr>` default — SVG2 behavior changed). |
| painting/opacity/50percent | 1 | css-color-4 allows percentage; test predates it. |
| structure/style-attribute | 1 | `<svg version="1.1">` disables geometry-in-style (SVG 1.1 behavior). |
| RenderOnly UB cases | 51 | Implementation-defined output; we verify no-crash only (per project policy, kept RenderOnly not Skip). |

---

## Template for new entries

```markdown
### Bn or Fn: Short title

**Impact:** N tests.

**Symptom:** (What does the diff look like?)

**Root cause:** (file:line if known; "needs investigation" otherwise.)

**Next step:** (Concrete action for a fix PR.)

**Affected tests:**
- path/to/first-test.svg
```
