#include <gmock/gmock.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>

#include "donner/base/tests/Runfiles.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

using testing::Combine;
using testing::ValuesIn;

namespace donner::svg {

using Params = ImageComparisonParams;

namespace {

// NOTE (Phase 4b increment 3): the per-binary `kActiveIsGeode` was removed in
// favor of a per-(test, ComparisonMode) model. The geode-specific gates below
// are stashed on the testcase (`ImageComparisonTestcase::geodeGate`) and applied
// by the fixture *only* for geode-backed modes (`GeodeGolden` /
// `GeodeTinyParity`), never for `TinyGolden`. So gate bodies here run only when
// the active mode is geode — no backend check is needed inside them.

/// Widen the per-pixel threshold for the geode comparison modes. Geode's 4×
/// MSAA rasterizer quantises edge coverage to 5 distinct alpha values (0, 64,
/// 128, 191, 255); tiny-skia's 16× supersample produces 17. The maximum
/// per-pixel alpha drift is therefore 1/16 ≈ 6.25%, which trips the default 2%
/// threshold on anti-aliased edges even though the shape geometry is identical.
/// Tests dominated by thin anti-aliased strokes — e.g. nested `<image>`
/// viewport frames in structure/image — need a ~10% threshold to absorb this
/// quantisation without inflating `maxMismatchedPixels`. Only ever called from a
/// `geodeGate`, which the fixture runs for geode modes only.
void widenThresholdForGeode(ImageComparisonParams& p, float threshold = 0.3f) {
  if (threshold > p.threshold) {
    p.threshold = threshold;
  }
}

/// Category-level auto-gate for the Geode backend. Geode is
/// feature-complete for fills/strokes/gradients/patterns/images/basic
/// shapes/compositing/clipping/masking/markers/blend-modes today —
/// filters (Phase 7) and text (Phase 4) still need to land before
/// the matching resvg categories can run under Geode.
///
/// This helper returns an `ImageComparisonParams` builder that, when
/// merged onto a testcase, cleanly skips it on Geode while leaving
/// CPU backends unaffected. The merge preserves any explicit Skip or
/// threshold override from the per-test overrides map so that we
/// don't over-widen on the CPU backends.
///
/// Returns an empty optional when the category has no Geode-specific
/// gate. The string_view inside the returned params has static
/// lifetime (string literal) so it can outlive the call.
std::optional<std::function<void(ImageComparisonParams&)>> geodeCategoryGate(
    std::string_view category) {
  // Filters: whole `filters/` tree (feBlend, feColorMatrix, feComposite,
  // feDiffuseLighting, feDropShadow, feImage, feTurbulence, filter,
  // filter-functions, flood-color, flood-opacity, enable-background, …)
  //
  // Exception: `filters/feGaussianBlur` runs on Geode (Phase 7 initial
  // scope) with a widened threshold for MSAA edge drift.
  if (category == "filters/feGaussianBlur") {
    return [](ImageComparisonParams& p) { widenThresholdForGeode(p); };
  }
  // Phase 7 extensions: feOffset, feColorMatrix, feFlood, feMerge, feComposite,
  // feBlend run on Geode with the same widened threshold for MSAA edge drift.
  if (category == "filters/feOffset" || category == "filters/feColorMatrix" ||
      category == "filters/feFlood" || category == "filters/feMerge" ||
      category == "filters/feComposite" || category == "filters/feBlend" ||
      category == "filters/feMorphology" || category == "filters/feComponentTransfer" ||
      category == "filters/feConvolveMatrix" || category == "filters/feTurbulence" ||
      category == "filters/feDisplacementMap" || category == "filters/feDiffuseLighting" ||
      category == "filters/feSpecularLighting" || category == "filters/feDropShadow" ||
      category == "filters/feImage" || category == "filters/feTile") {
    return [](ImageComparisonParams& p) { widenThresholdForGeode(p); };
  }
  if (category.rfind("filters/", 0) == 0 || category == "filters") {
    return [](ImageComparisonParams& p) {
      p.requireFeature(RendererBackendFeature::FilterEffects, "filter effects (Geode Phase 7)");
    };
  }

  // Text: whole `text/` tree plus the standalone shape-with-text cases.
  if (category.rfind("text/", 0) == 0 || category == "text") {
    return [](ImageComparisonParams& p) {
      p.requireFeature(RendererBackendFeature::Text, "text rendering (Geode Phase 4)");
    };
  }

  // Clip-paths (`masking/clip`, `masking/clipPath`, `masking/clip-rule`)
  // run through the Phase 3b mask pipeline, and `<mask>` elements run
  // through the Phase 3c mask-blit pipeline — no wholesale category
  // gate here. Individual per-file overrides handle any remaining
  // divergences.

  // The `painting/marker`, `painting/mix-blend-mode`, and `painting/isolation`
  // categories used to be wholesale-disabled on Geode because their combined
  // runtime pushed the whole variant past the 50-minute CI runner limit —
  // back-to-back llvmpipe runs died at exactly 51m in the Test step. That
  // slowdown was a symptom of issue #575 (wgpu-native pipeline accumulation
  // leaked ~1.6 MB per `RendererGeode`). With pipelines now pinned on
  // `GeodeDevice` the steady-state cost of a fresh renderer drops by ~18
  // pipelines and the full suite runs in ~45 s end-to-end on x86 — well
  // inside the runner's budget. The category gates are gone as of that fix;
  // any remaining marker / blend-mode / isolation failures are handled by
  // the per-filename overrides below.

  return std::nullopt;
}

/// Per-file auto-gate for the Geode backend. Many resvg tests live in
/// non-text categories but still embed text/tspan, markers, or
/// clip-path references — for example `painting/fill/on-text.svg` sits
/// under `painting/fill` but can only render once text lands. Gating
/// them by filename substring keeps the auto-gate close to the test
/// file without forcing us to hand-maintain a per-test override map
/// across every category.
///
/// `category` is the directory-relative category path
/// (e.g. `"structure/image"`) — used by rules that only apply inside
/// one category tree, like the nested-`<image>` preserveAspectRatio
/// threshold widening.
///
/// Returns a mutator that applies the right skip/feature bit, or
/// nullopt if no cross-category gate matches.
std::optional<std::function<void(ImageComparisonParams&)>> geodeFilenameGate(
    std::string_view category, std::string_view filename) {
  const auto contains = [&](std::string_view needle) {
    return filename.find(needle) != std::string_view::npos;
  };

  // Text-in-non-text-category: text children of fill/stroke/opacity/
  // visibility/display/pattern/<a> tests.
  if (contains("on-text") || contains("on-tspan") || contains("text-child") ||
      contains("with-text") || contains("with-a-text") || contains("optimizeSpeed-on-text")) {
    return [](ImageComparisonParams& p) {
      p.requireFeature(RendererBackendFeature::Text, "text rendering (Geode Phase 4)");
    };
  }

  // Marker-in-non-marker-category tests (overflow/shape-rendering
  // tests that reference `<marker>`) also render correctly — markers
  // are driver-level (see `painting/marker` note above), so these
  // need no filename gate on Geode.

  // Marker tests that render correctly on Geode but finish beyond the
  // default `maxMismatchedPixels=100` budget due to 4× MSAA / 16×
  // supersample AA drift on marker edges (and for `default-clip` +
  // `with-markerUnits=userSpaceOnUse`, integer-scissor vs fractional-
  // golden edges). The shape and clip are identical; only fractional
  // edge coverage differs. The standard `widenThresholdForGeode`
  // helper raises the per-pixel threshold to 0.3 which pulls every
  // marginal edge pixel under the "match" bar without masking real
  // regressions.
  if (category == "painting/marker" &&
      (filename == "marker-on-circle.svg" || filename == "with-an-image-child.svg")) {
    return [](ImageComparisonParams& p) { widenThresholdForGeode(p); };
  }

  // `painting/marker/{default-clip, with-a-large-stroke,
  // with-invalid-markerUnits, with-markerUnits=userSpaceOnUse}.svg` —
  // these render correctly on Geode (overflow-clip viewport, stroke-
  // width-driven marker scaling, userSpace fallbacks all work per
  // RendererDriver instrumentation) but finish with ~246 over-threshold
  // pixels along marker edge fringes because Geode's integer-scissor
  // clipping lands edge pixels on a different sub-pixel grid than the
  // golden's fractional-coordinate rasterization. Widen both the per-
  // pixel threshold and the max-count since threshold alone isn't
  // enough (246 > 100 default). The shape is identical; this is AA
  // drift, not structural divergence.
  if (category == "painting/marker" &&
      (filename == "default-clip.svg" || filename == "with-a-large-stroke.svg" ||
       filename == "with-invalid-markerUnits.svg" ||
       filename == "with-markerUnits=userSpaceOnUse.svg")) {
    return [](ImageComparisonParams& p) {
      widenThresholdForGeode(p);
      if (p.maxMismatchedPixels < 300) {
        p.maxMismatchedPixels = 300;
      }
    };
  }

  // feConvolveMatrix: Geode regressions still on this branch. The Phase 7
  // color-space conversion pass (commit ca392da9) flipped several convolve
  // tests green but didn't fix edge-mode / preserveAlpha / target-offset
  // handling. A follow-up pass on the convolve kernel is needed to land
  // parity with the tiny-skia reference.
  // TODO(geode): fix feConvolveMatrix edge-mode sampling, target offsets,
  // and preserveAlpha so these flip green without the disable.
  if (category == "filters/feConvolveMatrix" &&
      (filename == "edgeMode=none.svg" || filename == "edgeMode=wrap.svg" ||
       filename == "order=4.svg" || filename == "order=4-2.svg" || filename == "order=4-4.svg" ||
       filename == "preserveAlpha=true.svg" || filename == "targetX=0.svg" ||
       filename == "targetX=2.svg" || filename == "unset-order.svg")) {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode,
                       "TODO(geode): feConvolveMatrix edge-mode / targetX / "
                       "preserveAlpha / order=4 regressions");
    };
  }

  // Genuine Geode filter regressions. These are real pixel divergences
  // that predate issue #575's leak-fix and persist in isolation — the
  // failures were never about the leak, even though the leak's CI hang
  // surfaced them whack-a-mole style. Each needs its own investigation
  // pass; the disable stays until a dedicated fix lands.
  //
  // TODO(geode): fix these individually.
  //   - feMorphology/source-with-opacity.svg (~4.9k px)
  //   - feSpecularLighting/specularExponent=0.svg (~19.9k px on llvmpipe)
  //   - feTile/empty-region.svg (~65.5k px — entire canvas differs)
  //   - filter/transform-on-shape.svg (~1.3k px on llvmpipe)
  if ((category == "filters/feMorphology" && filename == "source-with-opacity.svg") ||
      (category == "filters/feSpecularLighting" && filename == "specularExponent=0.svg") ||
      (category == "filters/feTile" && filename == "empty-region.svg") ||
      (category == "filters/filter" && filename == "transform-on-shape.svg")) {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode,
                       "TODO(geode): genuine pixel regression — "
                       "needs follow-up investigation");
    };
  }

  // The painting/{opacity, fill-opacity, stroke, stroke-dasharray, overflow,
  // paint-order, shape-rendering} and text/font-variant entries that used to
  // sit on this block were added as fire-victims of issue #575's CI
  // watchdog — the pipeline-leak slowdown made them time out one at a time
  // as the run progressed. With the leak fixed on `geode-dev` they render
  // within budget and either pass outright or fall into an existing widening
  // rule elsewhere in this file. Re-enabling them uncovers real regressions
  // if any are still broken.

  // `orient=auto-on-M-L-Z.svg` still disagrees with the resvg/tiny-skia
  // reference at curve cusps — a real auto-orient tangent bug, not just
  // AA drift, so no threshold widening can absorb it (~624 px at the
  // strict default). The sibling `orient=auto-on-M-C-C-4.svg` now passes
  // at default threshold once its WithGoldenOverride is applied, so it
  // no longer needs the Geode disable. See F7b/c in the Phase 7 audit.
  if (category == "painting/marker" && filename == "orient=auto-on-M-L-Z.svg") {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode,
                       "TODO: Geode auto-orient marker tangent disagrees "
                       "at curve cusps (F7b/c)");
    };
  }

  // `structure/image/preserveAspectRatio=xMaxYMax-slice-on-svg` is
  // currently 104 pixels past the default 100-px max even at the
  // widened 0.3 per-pixel threshold. The failing pixels form a thin
  // fringe along the curved green stroke inside the embedded data-URL
  // SVG: 4× MSAA places the sub-pixel stroke edge on a different side
  // of the pixel boundary than tiny-skia's 16× supersample on roughly
  // 4% of the edge pixels, and those are 100% diffs (fully-coloured
  // stroke vs background), not something a threshold bump can absorb.
  // Historically this test was disabled under the mistaken "path
  // clipping (Phase 3)" rationale; Phase 3a polygon clipping confirms
  // no non-axis-aligned transform is in play (ancestor transform is
  // `matrix(16 0 0 16 10 175)`), so polygon clipping isn't the lever
  // to close this gap. Leaving disabled as an AA-quantisation TODO
  // until Geode picks up a finer sample pattern or analytic stroke AA.
  // TODO(geode): upgrade to 8× / 16× MSAA or analytic stroke AA to
  // shed the thin-stroke fringe pixels on nested-image tests.
  if (category == "structure/image" &&
      filename == "preserveAspectRatio=xMaxYMax-slice-on-svg.svg") {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode,
                       "4× MSAA thin-stroke fringe on nested image data URL");
    };
  }

  // `structure/image/preserveAspectRatio=*` nested `<image>` element tests.
  // Geode's 4× MSAA differs from tiny-skia's 16× supersample by up to
  // 6.25% per-pixel alpha on anti-aliased edges; the thin stroked
  // frames in these tests trip the default 2% threshold. Widen on
  // Geode only. The `structure/svg/preserveAspectRatio=*` sibling set
  // already passes at the default threshold because those tests
  // render a larger stroked frame at identical AA quantisation.
  if (category == "structure/image" && contains("preserveAspectRatio")) {
    return [](ImageComparisonParams& p) { widenThresholdForGeode(p); };
  }

  // `shapes/line/no-x1-coordinate`, `painting/stroke/control-points-clamping-1`,
  // and `preserveAspectRatio=xMaxYMax-slice-on-svg` all finish within
  // 2–6 pixels of the 100 max — the diff is the same 4× MSAA
  // quantisation as the preserveAspectRatio cluster but on a different
  // category path. Per-file widening.
  if (filename == "no-x1-coordinate.svg" || filename == "control-points-clamping-1.svg" ||
      filename == "preserveAspectRatio=xMaxYMax-slice-on-svg.svg") {
    return [](ImageComparisonParams& p) { widenThresholdForGeode(p); };
  }

  // `painting/display/bBox-impact.svg` clips a green rect through a
  // `<clipPath clipPathUnits="objectBoundingBox">` circle.  Geode's
  // 4× MSAA places the circular clip edge on a different sub-pixel
  // grid than tiny-skia's 16× supersample, producing ~111 fully-
  // opaque-vs-transparent boundary pixels (>30% per-pixel diff).
  // Widen both the per-pixel threshold and the max pixel count on
  // Geode only; the geometry is correct.
  if (category == "painting/display" && filename == "bBox-impact.svg") {
    return [](ImageComparisonParams& p) {
      widenThresholdForGeode(p);
      p.maxMismatchedPixels = std::max(p.maxMismatchedPixels, 150);
    };
  }

  // `tiny-pattern-upscaled`: 2×2 tile scaled 10× containing a circle.
  // With the 2× pattern supersample (matching TinySkia), the tile
  // texture is 40×40 texels. Remaining AA diff (~449 pixels) is from
  // Slug's 4× MSAA resolve vs TinySkia's software rasterizer — the
  // same quantisation gap handled by widenThresholdForGeode elsewhere.
  if (category == "paint-servers/pattern" && filename == "tiny-pattern-upscaled.svg") {
    return [](ImageComparisonParams& p) { widenThresholdForGeode(p); };
  }

  // feComponentTransfer/mixed-types: gradient input + mixed channel-function
  // types still diverges after the color-space conversion fix. Narrower
  // follow-up.
  if (category == "filters/feComponentTransfer" && filename == "mixed-types.svg") {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode,
                       "TODO(geode): feComponentTransfer gradient + mixed "
                       "per-channel types still diverges");
    };
  }

  // feDiffuseLighting/no-light-source renders a filter primitive that omits
  // the required `<feDistantLight>` / `<fePointLight>` / `<feSpotLight>`
  // child. SVG §15.22 doesn't clearly specify the fallback; tiny-skia
  // treats it as a no-op (pass-through input), but Geode produces a black
  // output (~230k-pixel diff on CI llvmpipe). Lavapipe happens to match
  // tiny-skia's output by accident. Separate follow-up.
  if (category == "filters/feDiffuseLighting" && filename == "no-light-source.svg") {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode,
                       "TODO(geode): feDiffuseLighting with no light-source "
                       "child draws black instead of no-op pass-through");
    };
  }

  // feImage subregion + preserveAspectRatio divergence between CI llvmpipe
  // and local lavapipe: these tests pass on lavapipe (the local dev driver)
  // but fail on CI's llvmpipe with large (28k-36k-pixel) diffs, indicating
  // the feImage subregion+aspect-ratio placement path produces different
  // output depending on the Vulkan driver's texture-sampling behavior.
  // Separate follow-up — the feImage rendering path needs to be tightened
  // to be driver-independent.
  if (category == "filters/feImage" &&
      (filename == "preserveAspectRatio=none.svg" || filename == "with-subregion-1.svg" ||
       filename == "with-subregion-2.svg" || filename == "with-subregion-3.svg" ||
       filename == "with-subregion-4.svg")) {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode,
                       "TODO(geode): feImage subregion / preserveAspectRatio "
                       "placement diverges between Mesa llvmpipe and lavapipe");
    };
  }

  // `filters/filter/with-region-and-subregion.svg`,
  // `filters/filter/with-subregion-1.svg`, and the
  // `filters/filter/subregion-and-primitiveUnits=objectBoundingBox-{1,2}.svg`
  // tests exercise primitive-subregion clipping. The subregion math now
  // matches the CPU path's floor/ceil pixel round-out, but the gray
  // crosshair and 0.5px stroked frame in these scenes still trip the
  // 2% per-pixel default on Geode's 4× MSAA quantisation grid (the
  // same AA drift documented for the marker / preserveAspectRatio
  // tests above). The blurred green primitive itself is pixel-accurate;
  // widening the per-pixel threshold to 0.3 absorbs the AA fringe
  // pixels and the small body diff the blur picks up at the
  // half-pixel-aligned subregion edges. The shape and clip are
  // identical; only fractional edge coverage differs.
  if (category == "filters/filter" &&
      (filename == "with-region-and-subregion.svg" || filename == "with-subregion-1.svg" ||
       filename == "subregion-and-primitiveUnits=objectBoundingBox-1.svg" ||
       filename == "subregion-and-primitiveUnits=objectBoundingBox-2.svg")) {
    return [](ImageComparisonParams& p) {
      widenThresholdForGeode(p);
      if (p.maxMismatchedPixels < 2500) {
        p.maxMismatchedPixels = 2500;
      }
    };
  }

  // feMorphology/huge-radius renders a morphology with `radius="50"` which
  // the (separable, scalar-loop) WGSL kernel iterates ~101× per axis per
  // texel. Under Mesa llvmpipe (software Vulkan on CI) that runs ~25-35 s
  // — consistently over the 30 s per-testcase watchdog installed by
  // //donner/base:gtest_timeout_main. Lavapipe on the dev host finishes
  // in ~23 s, which is why it passed locally. Either tighten the kernel
  // (workgroup-size tuning / storage-buffer separable pass) or gate the
  // test to non-software Vulkan; until then, disable on Geode so CI
  // doesn't trip the watchdog.
  if (category == "filters/feMorphology" && filename == "huge-radius.svg") {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode,
                       "TODO(geode): huge-radius morphology > 30 s on Mesa "
                       "llvmpipe CI — trips per-testcase watchdog");
    };
  }

  // `masking/clipPath/simple-case.svg` clips a green rect through a
  // 5-point star path. Geode's 4× MSAA places the star's acute-angle
  // edge pixels on a different sub-pixel grid than tiny-skia's 16×
  // supersample, producing ~728 fully-coloured boundary pixels along
  // the star's points (>30% per-pixel diff vs the soft-AA reference).
  // The shape and clip are identical; only fractional edge coverage
  // along the acute tips differs.
  // TODO(geode): upgrade to 8× / 16× MSAA or analytic stroke AA to
  // shed the acute-angle fringe pixels on star-clip tests.
  if (category == "masking/clipPath" && filename == "simple-case.svg") {
    return [](ImageComparisonParams& p) {
      widenThresholdForGeode(p);
      p.maxMismatchedPixels = std::max(p.maxMismatchedPixels, 900);
    };
  }

  return std::nullopt;
}

/// Parity-only gate for the Geode backend (Phase 4b final policy). The
/// GeodeTinyParity mode renders geode + tiny-skia and pixelmatches them at the
/// suite's default 0.02 threshold with a FLAT 100-px budget (no per-test
/// thresholds). Tests whose geode-vs-tiny diff exceeds 100 px fall into two
/// inventoried buckets, both gated here (the diff is real and tracked, never
/// absorbed by a larger budget):
///
///  - EDGE-FLOOR (172): the proven 4x MSAA edge-coverage quantization (101-763
///    px hugging glyph/shape edges). These ratchet out together when Geode gains
///    finer AA. Tracked: docs/design_docs/0017 §Phase 4b.
///  - GENUINE (56): real geode-vs-tiny divergences. Filter color/algorithm bugs
///    reference 0021 §G2; text (incl. text-on-shape) reference 0038.
///
/// Keyed on `"<category>/<filename>"`. Returns a parity-disable mutator or
/// nullopt. Applied only to the parity instance — TinyGolden / GeodeGolden are
/// untouched.
std::optional<std::function<void(ImageComparisonParams&)>> geodeParityGate(
    std::string_view category, std::string_view filename) {
  const std::string key = std::string(category) + "/" + std::string(filename);

  // ── EDGE-FLOOR: accepted by-design geode-vs-tiny AAA coverage delta (0039 §13) ──────────
  static const std::set<std::string_view> kEdgeFloor = {
      // feImage/embedded-png recategorized from kGenuineG2 (2026-05-27 — see
      // 0021 §G2): geode places the data-URI image *correctly* (zero-diff
      // interior); the residual ~1580px is a 1px-wide edge band around the image
      // rectangle perimeter (geode bilinear vs tiny RasterizeTransformedImage edge
      // sampling). It is order-independent (no idempotency bug — unlike the 8
      // fragment-reference feImage cases that were fixed). Edge-coverage, not a
      // placement/color bug.
      "filters/feImage/embedded-png.svg",
      // Recategorized from kGenuineG2 (2026-05-25 — see 0021 §G2): geode renders
      // both *correctly*; the residual is a render-correct quantization floor, not
      // a structural bug.
      //   feColorMatrix/non-normalized-values (~2786px): edge-coverage — a thin
      //   vertical band at the rect's anti-aliased left edge where partial coverage
      //   (alpha ~85-91) is amplified by the extreme matrix coefficients (50,-100).
      //   feConvolveMatrix/custom-divisor (~4464px): the convolve math matches tiny
      //   bit-for-bit and the render is visually identical; the residual is the
      //   premultiplied-low-alpha rounding (e.g. B 0 vs 15 at a=35) at the many
      //   pattern-cell seams — same class as the G5 premultiply fills.
      "filters/feColorMatrix/type=matrix-with-non-normalized-values.svg",
      "filters/feConvolveMatrix/custom-divisor.svg", "filters/feColorMatrix/type=saturate.svg",
      "filters/feDropShadow/only-stdDeviation.svg",
      "filters/filter/subregion-and-primitiveUnits=objectBoundingBox-1.svg",
      "filters/filter/subregion-and-primitiveUnits=objectBoundingBox-2.svg",
      "masking/mask/on-a-small-object.svg", "paint-servers/pattern/tiny-pattern-upscaled.svg",
      "painting/display/bBox-impact.svg", "painting/marker/with-a-text-child.svg",
      "painting/marker/with-an-image-child.svg", "painting/stroke/control-points-clamping-1.svg",
      "painting/visibility/collapse-on-tspan.svg", "painting/visibility/hidden-on-tspan.svg",
      "shapes/line/no-y1-coordinate.svg",
      "structure/image/preserveAspectRatio=xMaxYMax-meet-on-svg.svg",
      "structure/image/preserveAspectRatio=xMaxYMax-slice.svg",
      "structure/image/preserveAspectRatio=xMidYMid-meet-on-svg.svg",
      "structure/image/preserveAspectRatio=xMidYMid-slice-on-svg.svg",
      "structure/image/preserveAspectRatio=xMidYMid-slice.svg",
      "structure/image/preserveAspectRatio=xMinYMin-meet-on-svg.svg",
      "structure/image/preserveAspectRatio=xMinYMin-slice-on-svg.svg",
      "structure/image/preserveAspectRatio=xMinYMin-slice.svg",
      "structure/svg/preserveAspectRatio-with-viewBox-not-at-zero-pos.svg",
      "structure/svg/preserveAspectRatio=none.svg",
      "structure/svg/preserveAspectRatio=xMaxYMax.svg",
      "structure/svg/preserveAspectRatio=xMidYMid.svg",
      "structure/svg/preserveAspectRatio=xMinYMin.svg", "structure/svg/proportional-viewBox.svg",
      "text/alignment-baseline/alphabetic.svg", "text/alignment-baseline/auto.svg",
      "text/alignment-baseline/central.svg",
      "text/alignment-baseline/hanging-and-baseline-shift-eq-20-on-tspan.svg",
      "text/alignment-baseline/hanging-on-tspan.svg",
      "text/alignment-baseline/hanging-with-underline.svg", "text/alignment-baseline/hanging.svg",
      "text/alignment-baseline/inherit.svg", "text/alignment-baseline/mathematical.svg",
      "text/baseline-shift/-10.svg", "text/baseline-shift/-50percent.svg",
      "text/baseline-shift/0.svg", "text/baseline-shift/10.svg", "text/baseline-shift/2mm.svg",
      "text/baseline-shift/50percent.svg", "text/baseline-shift/baseline.svg",
      "text/baseline-shift/inheritance-1.svg", "text/baseline-shift/inheritance-2.svg",
      "text/baseline-shift/inheritance-3.svg", "text/baseline-shift/inheritance-4.svg",
      "text/baseline-shift/inheritance-5.svg", "text/baseline-shift/invalid-value.svg",
      "text/baseline-shift/sub.svg", "text/baseline-shift/super.svg",
      "text/baseline-shift/with-rotate.svg", "text/dominant-baseline/alphabetic.svg",
      "text/dominant-baseline/auto.svg", "text/dominant-baseline/central.svg",
      "text/dominant-baseline/different-alignment-baseline-on-tspan.svg",
      "text/dominant-baseline/equal-alignment-baseline-on-tspan.svg",
      "text/dominant-baseline/ideographic.svg", "text/dominant-baseline/mathematical.svg",
      "text/font-kerning/as-property.svg", "text/lengthAdjust/spacingAndGlyphs.svg",
      "text/text-anchor/coordinates-list.svg", "text/text-anchor/end-on-text.svg",
      "text/text-anchor/end-with-letter-spacing.svg", "text/text-anchor/inheritance-1.svg",
      "text/text-anchor/inheritance-2.svg", "text/text-anchor/inheritance-3.svg",
      "text/text-anchor/invalid-value-on-text.svg", "text/text-anchor/middle-on-text.svg",
      "text/text-anchor/on-the-first-tspan.svg", "text/text-anchor/on-tspan-with-arabic.svg",
      "text/text-anchor/on-tspan.svg", "text/text-anchor/start-on-text.svg",
      "text/text-anchor/text-anchor-not-on-text-chunk.svg",
      "text/text-decoration/all-types-inline-comma-separated.svg",
      "text/text-decoration/all-types-inline-no-spaces.svg",
      "text/text-decoration/all-types-inline.svg", "text/text-decoration/all-types-nested.svg",
      "text/text-decoration/indirect.svg", "text/text-decoration/line-through.svg",
      "text/text-decoration/outside-the-text-element.svg", "text/text-decoration/overline.svg",
      "text/text-decoration/style-resolving-1.svg", "text/text-decoration/style-resolving-2.svg",
      "text/text-decoration/style-resolving-3.svg", "text/text-decoration/style-resolving-4.svg",
      "text/text-decoration/underline-with-dy-list-1.svg",
      "text/text-decoration/underline-with-rotate-list-3.svg",
      "text/text-decoration/underline-with-y-list.svg", "text/text-decoration/underline.svg",
      "text/text-rendering/geometricPrecision.svg", "text/text-rendering/on-tspan.svg",
      "text/text-rendering/optimizeLegibility.svg", "text/text-rendering/optimizeSpeed.svg",
      "text/text-rendering/with-underline.svg", "text/text/dx-and-dy-instead-of-x-and-y.svg",
      "text/text/dx-and-dy-with-less-values-than-characters.svg",
      "text/text/dx-and-dy-with-more-values-than-characters.svg",
      "text/text/dx-and-dy-with-multiple-values.svg", "text/text/em-and-ex-coordinates.svg",
      "text/text/escaped-text-1.svg", "text/text/escaped-text-2.svg",
      "text/text/escaped-text-3.svg", "text/text/escaped-text-4.svg",
      "text/text/mm-coordinates.svg", "text/text/nested.svg", "text/text/no-coordinates.svg",
      "text/text/rotate-with-an-invalid-angle.svg",
      "text/text/rotate-with-less-values-than-characters.svg",
      "text/text/rotate-with-more-values-than-characters.svg",
      "text/text/rotate-with-multiple-values-underline-and-pattern.svg",
      "text/text/rotate-with-multiple-values.svg", "text/text/rotate.svg",
      "text/text/simple-case.svg", "text/text/transform.svg",
      "text/text/x-and-y-with-dx-and-dy-lists.svg", "text/text/x-and-y-with-dx-and-dy.svg",
      "text/text/x-and-y-with-less-values-than-characters.svg",
      "text/text/x-and-y-with-more-values-than-characters.svg",
      "text/text/x-and-y-with-multiple-values-and-tspan.svg",
      "text/text/x-and-y-with-multiple-values.svg", "text/text/xml-lang=ja.svg",
      "text/text/xml-space.svg", "text/textLength/150-on-parent.svg",
      "text/textLength/150-on-tspan.svg", "text/textLength/150.svg", "text/textLength/40mm.svg",
      "text/textLength/75percent.svg", "text/textLength/inherit.svg",
      "text/textLength/negative.svg", "text/textPath/m-L-Z-path.svg",
      "text/tref/link-to-a-non-SVG-element.svg", "text/tref/nested.svg",
      "text/tspan/mixed-xml-space-1.svg", "text/tspan/mixed-xml-space-2.svg",
      "text/tspan/mixed.svg", "text/tspan/multiple-coordinates.svg",
      "text/tspan/nested-whitespaces.svg", "text/tspan/nested.svg", "text/tspan/only-with-y.svg",
      "text/tspan/outside-the-text.svg", "text/tspan/pseudo-multi-line.svg",
      "text/tspan/rotate-and-display-none.svg", "text/tspan/rotate-on-child.svg",
      "text/tspan/sequential.svg", "text/tspan/style-override.svg",
      "text/tspan/text-shaping-across-multiple-tspan-1.svg",
      "text/tspan/text-shaping-across-multiple-tspan-2.svg", "text/tspan/transform.svg",
      "text/tspan/with-dy.svg", "text/tspan/with-opacity.svg", "text/tspan/with-x-and-y.svg",
      "text/tspan/without-attributes.svg", "text/tspan/xml-space-1.svg",
      "text/tspan/xml-space-2.svg", "text/word-spacing/-5.svg", "text/word-spacing/0.svg",
      "text/word-spacing/10.svg", "text/word-spacing/2mm.svg", "text/word-spacing/5percent.svg",
      "text/word-spacing/normal.svg", "text/writing-mode/horizontal-tb.svg",
      "text/writing-mode/inheritance.svg", "text/writing-mode/invalid-value.svg",
      "text/writing-mode/lr-tb.svg", "text/writing-mode/lr.svg", "text/writing-mode/on-tspan.svg",
      "text/writing-mode/rl-tb.svg", "text/writing-mode/rl.svg", "text/writing-mode/tb-rl.svg",
      "text/writing-mode/tb-with-alignment.svg", "text/writing-mode/tb.svg",
      // Recategorized from kGenuineText (audited 2026-05-26 — see 0038): geode
      // renders these *correctly* (right glyphs/positions/colors); the diff is the
      // 4x MSAA edge fringe, just over 100px from large cumulative small-glyph
      // perimeter (many lines / long strings / tiled or on-path text) — same class
      // as the other edge-floor entries, not a structural bug.
      "paint-servers/pattern/text-child.svg",        // tiled pattern-of-text, correct
      "text/font-size/named-value.svg",              // 11 small lines, correct
      "text/letter-spacing/on-Arabic.svg",           // correct glyphs, edge fringe
      "text/text-decoration/tspan-decoration.svg",   // correct multi-color + underline
      "text/textPath/dy-with-tiny-coordinates.svg",  // correct on-path placement
      // Recategorized from kGenuineText after the paint(b) fix (2026-05-26 — see 0038):
      // geode now renders these gradient-on-text cases *correctly*; the residual is the
      // 4x MSAA edge fringe (small 24px gradient text / gradient stroke ring).
      "painting/stroke/linear-gradient-on-text.svg",  // gradient stroke ring, ~465px fringe
      "text/tspan/tspan-bbox-1.svg",                  // 24px gradient span, ~702px fringe
      "text/tspan/tspan-bbox-2.svg",                  // 24px gradient span, ~694px fringe
      // Recategorized from kGenuineText after the baseline-shift fix (2026-05-26 — see
      // 0038): the nested baseline-shift accumulation bug (shared TextEngine layout) is
      // fixed; these now render correctly at 64px, residual = 4x MSAA edge fringe.
      "text/baseline-shift/deeply-nested-super.svg",     // ~720px fringe
      "text/baseline-shift/mixed-nested.svg",            // ~690px fringe
      "text/baseline-shift/nested-length.svg",           // ~686px fringe
      "text/baseline-shift/nested-super.svg",            // ~677px fringe
      "text/baseline-shift/nested-with-baseline-1.svg",  // ~702px fringe
      "text/baseline-shift/nested-with-baseline-2.svg",  // ~702px fringe
      // Recategorized from kGenuineText (2026-05-24 — see 0038): the last two
      // STRUCTURAL text gates. Diff-PNG audit shows geode places the per-char
      // dy-staircase / per-glyph rotation, gradient fill, gray stroke, and
      // underline *correctly* (zero-diff glyph interiors). The residual is pure
      // edge fringe — ~480px above the plain-black siblings (underline-with-dy-
      // list-1 699px / -rotate-list-3 686px, already edge-floor) only because the
      // gray text-stroke ring doubles each glyph's perimeter and the gradient adds
      // edge variation. tiny re-render is idempotent (0px), so no double-draw bug.
      "text/text-decoration/underline-with-dy-list-2.svg",      // ~1177px fringe
      "text/text-decoration/underline-with-rotate-list-4.svg",  // ~1145px fringe
  };
  if (kEdgeFloor.count(key)) {
    return [](ImageComparisonParams& p) {
      // ACCEPTED by-design (0039 §13): geode's edge coverage vs tiny-skia's Skia-AAA
      // scanline coverage differ by a sub-perceptual ~1px AA fringe, plus the resvg
      // template's 0.5px crosshair sub-pixel placement (tiny's snapY quarter-pixel
      // quantization). The CONTENT matches (glyph fringe is correctly AA-excluded by
      // pixelmatch); unfixable in-renderer (AAA is a stateful CPU scanline accumulator,
      // not per-fragment GPU-replicable -- proven across 4 attempts, see 0039 §§8-13).
      // NOT 4x MSAA quantization (sample-independent: identical at 16x/64x).
      p.disableGeodeParity("geode-vs-tiny AAA coverage/crosshair sub-pixel delta; content "
                           "matches, unfixable in-renderer (accepted by-design: 0039 §13)");
    };
  }

  // ── GENUINE: filter color/algorithm divergences (0021 §G2) ─────────────────
  static const std::set<std::string_view> kGenuineG2 = {
      // feComposite-arithmetic + feComponentTransfer (8) un-gated 2026-05-27:
      // root cause was geode running these two primitives in sRGB while tiny-skia
      // runs them in linearRGB (the `color-interpolation-filters` default). Fixed
      // by wrapping both with the existing sRGB↔linear conversion in
      // GeodeFilterEngine::execute (same as feGaussianBlur/feColorMatrix/feBlend);
      // both now pass geode-vs-tiny parity at 0px. See 0021 §G2.
      //
      // feImage (9) un-gated 2026-05-27: 8 fragment-reference cases (link-to-*,
      // chained-feImage) were a shared-code re-draw idempotency bug, NOT a geode
      // placement bug — `preRenderFeImageFragments` leaked OffscreenFeImage shadow
      // RenderingInstanceComponents into the global pool, so the 2nd render of a
      // document drew the referenced fragment as main content (corrupting BOTH
      // backends). Fixed by filtering those instances out of the main snapshot in
      // RendererDriver::draw (+ red→green test FeImageFragmentRedrawIsIdempotent);
      // all 8 now pass parity ≤1px. The 9th (embedded-png) was a 1px image-edge
      // band → moved to kEdgeFloor. See 0021 §G2.
      //
      // feTurbulence (12) un-gated 2026-05-27: NOT an inherent noise-algorithm
      // mismatch (the audit's "different noise impl" was wrong). geode's WGSL
      // already implements the spec-exact Park-Miller RNG + gradient/lattice
      // tables (matching tiny-skia bit-for-bit). The only deviation was a missing
      // linearRGB→sRGB conversion: feTurbulence generates noise in the filter's
      // color-interpolation-filters space (linearRGB by default) and the result is
      // stored in sRGB; tiny runs `linearToSrgb` on the generated noise, geode did
      // not (e.g. fractalNoise center 128 vs tiny 187 = sRGB(0.5)). Fixed by a
      // one-way linear→sRGB conversion of the turbulence output in
      // GeodeFilterEngine::execute. All 12 now pass parity at 0px. See 0021 §G2.
      //
      // linearRGB sweep (5) un-gated 2026-05-25: feMerge, feConvolveMatrix,
      // feDiffuse/feSpecularLighting did not wrap their primitive in the
      // sRGB↔linear conversion that `color-interpolation-filters: linearRGB`
      // (the default) requires — the same root as the composite/componentTransfer/
      // turbulence fixes. Added the wraps in GeodeFilterEngine::execute (feMerge
      // composites each input in linear; lighting converts the light color
      // sRGB→linear + output linear→sRGB). feSpecularLighting/specularExponent=256
      // ALSO needed the spec clamp of specularExponent to [1,128] (its filter is
      // color-interpolation-filters=sRGB, so the clamp — not linearRGB — was its
      // bug). Results (geode↔tiny): feDiffuseLighting 221435→0, specularExponent
      // 768→0, feMerge/linearRGB 1100→11, feMerge/complex-transform 1325→35, and
      // filter/on-group-outside-canvas was already 0. All ≤100, un-gated.
      //
      // feGaussianBlur/complex-transform stays gated — NOT color-space (blur
      // already wraps linearRGB). Under a skewed ancestor transform the blurred
      // region's device-space extent/placement diverges from tiny (a solid ~35k px
      // frame around the rotated blurred rect, not edge fringe) — a filter-region/
      // CTM-projection issue. Deferred as a distinct root. See 0021 §G2.
      "filters/feGaussianBlur/complex-transform.svg",
  };
  if (kGenuineG2.count(key)) {
    return [](ImageComparisonParams& p) {
      p.disableGeodeParity("geode filter divergence vs tiny-skia (tracked: 0021 G2)");
    };
  }

  // ── GENUINE: text / text-on-shape divergences (0038 catalog) ───────────────
  // STRUCTURAL geode-vs-tiny text divergences — geode renders *wrong* (whole-glyph
  // offset / wrong paint), not the 4x MSAA edge fringe. **Empty as of 2026-05-24:**
  // every catalogued text divergence has been resolved (paint(b), baseline-shift,
  // per-char dy/rotate). The last two (underline-with-dy-list-2 / -rotate-list-4)
  // were audited by diff PNG — geode places the per-char dy-staircase / per-glyph
  // rotation, gradient fill, gray stroke, and underline *correctly* (zero-diff glyph
  // interiors); the residual (1177 / 1145 px) is pure edge fringe, ~480px above the
  // plain-black siblings (dy-list-1 699px / rotate-list-3 686px, both already
  // edge-floor) solely because the gray text-stroke ring doubles each glyph's
  // perimeter and the gradient adds edge variation — same class as the already-
  // recategorized gradient-on-text / stroke-ring cases. Moved to kEdgeFloor; not a
  // structural bug. tiny re-render is idempotent (0px), so no double-draw/state-
  // accumulation bug. See 0038. Kept (empty) for future text divergences.
  static const std::set<std::string_view> kGenuineText = {};
  if (kGenuineText.count(key)) {
    return [](ImageComparisonParams& p) {
      p.disableGeodeParity("geode text divergence vs tiny-skia (tracked: 0038)");
    };
  }

  return std::nullopt;
}

// Discover every .svg test in one category directory under the resvg-test-suite
// tree. Example: getTestsInCategory("painting/fill") → all .svg files in
// <runfiles>/resvg-test-suite/tests/painting/fill/.
//
// Overrides is keyed by the bare filename (e.g. "rgb-int-int-int.svg") and
// picks per-test params (Skip, threshold, golden override, etc). Any file not
// in the overrides map uses defaultParams. When the category matches a
// Geode-blocked feature (filters, text), every resulting testcase is also tagged with the matching
// `requireFeature` / `disableBackend` bit so the Geode backend skips cleanly
// while the CPU variants still run the existing coverage.
std::vector<ImageComparisonTestcase> getTestsInCategory(
    std::string_view category, std::map<std::string, ImageComparisonParams> overrides = {},
    ImageComparisonParams defaultParams = {}) {
  const std::string kTestsRoot =
      Runfiles::instance().RlocationExternal("resvg-test-suite", "tests");
  const std::filesystem::path kCategoryDir = std::filesystem::path(kTestsRoot) / category;

  std::vector<ImageComparisonTestcase> testPlan;
  if (!std::filesystem::exists(kCategoryDir)) {
    return testPlan;
  }

  const auto categoryGate = geodeCategoryGate(category);

  for (const auto& entry : std::filesystem::directory_iterator(kCategoryDir)) {
    if (entry.path().extension() != ".svg") {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    ImageComparisonTestcase test;
    test.svgFilename = entry.path();
    test.params = defaultParams;

    if (auto it = overrides.find(filename); it != overrides.end()) {
      test.params = it->second;
    }

    // Stash the Geode-specific gates (category + per-file) as a deferred
    // mutator on the testcase. The fixture applies it at runtime *only* for
    // geode-backed modes (GeodeGolden / GeodeTinyParity), never for TinyGolden:
    // `requireFeature` / `disableBackend` then make the geode mode skip cleanly
    // and `widenThresholdForGeode` widens its threshold, while the TinyGolden
    // mode keeps the strict golden thresholds. Combining both gates here keeps
    // the historical merge order (category first, then per-file).
    auto filenameGate = geodeFilenameGate(category, filename);
    // Parity gate sets `disableGeodeTinyParity` for the inventoried >100px
    // geode-vs-tiny divergences (4x MSAA edge floor + genuine bugs). It only
    // affects the GeodeTinyParity instance; GeodeGolden / TinyGolden ignore the
    // flag. Folded into `geodeGate` (which the fixture applies for geode modes).
    auto parityGate = geodeParityGate(category, filename);
    if (categoryGate || filenameGate || parityGate) {
      test.geodeGate = [categoryGate, filenameGate, parityGate](ImageComparisonParams& p) {
        if (p.skip) {
          return;  // Explicit Skip() takes precedence; don't re-gate.
        }
        if (categoryGate) {
          (*categoryGate)(p);
        }
        // Mirror the historical guard: the per-file gate only ran when the
        // category gate hadn't already turned the test into a Skip.
        if (!p.skip && filenameGate) {
          (*filenameGate)(p);
        }
        if (parityGate) {
          (*parityGate)(p);
        }
      };
    }

    // Canvas size matches the resvg-test-suite reference renderings. This is a
    // base param (mode-independent), so it stays on `test.params`.
    test.params.setCanvasSize(500, 500);

    testPlan.emplace_back(std::move(test));
  }

  std::sort(testPlan.begin(), testPlan.end());
  return testPlan;
}

}  // namespace

TEST_P(ImageComparisonTestFixture, ResvgTest) {
  const ImageComparisonTestcase& testcase = std::get<0>(GetParam());

  // In the post-rename layout goldens sit next to their .svg in the same
  // category directory, unless overridden by WithGoldenOverride().
  std::filesystem::path goldenFilename;
  if (testcase.params.overrideGoldenFilename.empty()) {
    goldenFilename = testcase.svgFilename;
    goldenFilename.replace_extension(".png");
  } else {
    goldenFilename = testcase.params.overrideGoldenFilename;
  }

  SVGDocument document = loadSVG(testcase.svgFilename.string().c_str(),
                                 Runfiles::instance().RlocationExternal("resvg-test-suite", ""));
  renderAndCompare(document, testcase.svgFilename, goldenFilename.string().c_str());
}

// ----------------------------------------------------------------------------
// G1 Geode text-decoration parity repro
// (see docs/design_docs/0021-resvg_feature_gaps.md §Geode parity).
//
// Root cause (confirmed in code + pixels): RendererGeode::drawText only fills
// glyph outlines -- it renders neither text strokes nor text-decoration
// (underline / overline / line-through), while RendererTinySkia::drawText
// renders both (see RendererTinySkia.cc, "Draw text-decoration lines"). This is
// the largest structural Geode text-parity gap: the whole text/text-decoration
// category (~44 tests) fails because the decoration line is simply missing.
//
// This is NOT the 4x-MSAA edge-sampling difference that dominates the rest of
// the text suite (that is a deliberate perf tradeoff -- Geode stays at 4x). The
// repro is fill-only and integer-positioned so the glyph-edge MSAA difference
// stays within the default budget; the missing underline is the only structural
// divergence. tiny-skia authors and passes the golden; Geode fails until
// drawText draws decorations.
//
// Authoring the golden (from tiny-skia):
//   UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
//     bazel run //donner/svg/renderer/tests:resvg_test_suite_default_text \
//       -- --gtest_filter='*GeodeTextDecorationRepro*'
// ----------------------------------------------------------------------------
class GeodeTextDecorationRepro : public ImageComparisonTestFixture {};

TEST_F(GeodeTextDecorationRepro, UnderlineNotRenderedOnGeode) {
  // Runs on both backends: tiny-skia authors/passes the golden, Geode fails on
  // the missing underline. Fonts come from the resvg suite's fonts/ dir.
  const std::filesystem::path resvgRoot =
      Runfiles::instance().RlocationExternal("resvg-test-suite", "");
  const char* svg = "donner/svg/renderer/testdata/geode_text_decoration_underline.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/geode_text_decoration_underline.png";

  SVGDocument document = loadSVG(svg, resvgRoot);

  ImageComparisonParams params = Params::WithThreshold(kDefaultThreshold, kDefaultMismatchedPixels);
  params.enableGoldenUpdateFromEnv();
  renderAndCompare(document, svg, golden, params);
}

// ----------------------------------------------------------------------------
// G1 Geode pattern-on-text parity repro
// (see docs/design_docs/0021-resvg_feature_gaps.md §Geode parity).
//
// Root cause (confirmed in code + pixels): a `<text fill="url(#pattern)">`
// stages a pattern paint slot via the driver's renderPattern/endPatternTile,
// to be consumed by the next fill. RendererTinySkia::drawText consumes it
// (clipping the pattern to the glyph outlines). RendererGeode::drawText had no
// pattern-fill path: resolveSpanFill resolves a pattern ref to alpha=0, so the
// glyph fill was skipped AND the staged `patternFillPaint` slot was never reset
// -- it then leaked onto the NEXT drawn shape's `fillResolved`, painting the
// pattern across that shape (e.g. a full-canvas frame rect).
//
// The repro fills "Text" with a checkerboard pattern and follows it with a
// solid-black rect that must stay solid (the leak symptom). tiny-skia authors
// and passes the golden; Geode failed until drawText consumed the slot.
//
// Authoring the golden (from tiny-skia):
//   UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
//     bazel run //donner/svg/renderer/tests:resvg_test_suite_default_text \
//       -- --gtest_filter='*PatternFillOnTextLeaksOnGeode*'
// ----------------------------------------------------------------------------
TEST_F(GeodeTextDecorationRepro, PatternFillOnTextLeaksOnGeode) {
  const std::filesystem::path resvgRoot =
      Runfiles::instance().RlocationExternal("resvg-test-suite", "");
  const char* svg = "donner/svg/renderer/testdata/geode_text_pattern_fill.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/geode_text_pattern_fill.png";

  SVGDocument document = loadSVG(svg, resvgRoot);

  ImageComparisonParams params = Params::WithThreshold(kDefaultThreshold, kDefaultMismatchedPixels);
  params.enableGoldenUpdateFromEnv();
  renderAndCompare(document, svg, golden, params);
}

// ----------------------------------------------------------------------------
// Nested baseline-shift re-draw idempotency regression (docs/design_docs/0038).
//
// `resolvePerSpanLayoutStyles` appended to `span.ancestorBaselineShifts` via
// push_back without clearing, and runs on every `draw()`. A SECOND render of the
// same `SVGDocument` / `ComputedTextComponent` therefore DOUBLED the nested
// baseline-shift. The parity harness (two backends drawing the same document)
// surfaced it, but it's a real production bug for any re-draw (e.g. editor
// re-renders). This test pins it directly: render the same document twice via
// tiny-skia and require pixel-identity. Both draws share the 4x MSAA edge floor,
// so an idempotent layout makes them byte-identical (0 diff); the doubled-shift
// bug shifts the glyphs in draw #2, producing a large diff.
//
// Backend-agnostic: renders via tiny-skia (always linked), so it runs on the
// default CPU build and exercises the shared TextEngine layout, not a backend.
// ----------------------------------------------------------------------------
TEST_F(GeodeTextDecorationRepro, NestedBaselineShiftRedrawIsIdempotent) {
  const std::filesystem::path resvgRoot =
      Runfiles::instance().RlocationExternal("resvg-test-suite", "");
  const char* svg = "donner/svg/renderer/testdata/text_nested_baseline_shift_idempotency.svg";

  SVGDocument document = loadSVG(svg, resvgRoot);

  // Two renders of the SAME document. With the layout-idempotency bug, draw #2's
  // nested baseline-shift is doubled, so its glyphs sit higher than draw #1's.
  const RendererBitmap first = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  const RendererBitmap second = RenderDocumentWithBackend(document, RendererBackend::TinySkia);

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());

  // Strict identity: the two renders must be pixel-for-pixel identical.
  ExpectBitmapsIdentical(second, first, "nested_baseline_shift_redraw");
}

// ----------------------------------------------------------------------------
// feImage fragment re-draw idempotency regression (docs/design_docs/0021 §G2).
//
// `RendererDriver::preRenderFeImageFragments` instantiates a referenced fragment
// (`#rect3`) as an OffscreenFeImage shadow tree; `createFeImageShadowTree`
// emplaces a `RenderingInstanceComponent` per shadow entity into the global pool
// so the feImage pre-pass can rasterize it. Those instances are offscreen-only,
// but the render-tree fast path skips a rebuild when nothing is dirty, so they
// lingered in the pool between renders. A SECOND render of the same SVGDocument
// then picked them up in the main-entity snapshot and drew the fragment as if it
// were main content (e.g. the green rect stamped at its document position on top
// of the filtered output), so render #2 differed markedly from render #1. The
// geode-vs-tiny parity harness (which draws the same document twice) surfaced it,
// but it's a real bug for any re-draw (e.g. editor re-renders) and it corrupts
// BOTH backends. The fix filters OffscreenFeImage shadow instances out of the
// main snapshot (RendererDriver::collectOffscreenFeImageShadowEntities).
//
// Backend-agnostic: renders via tiny-skia (always linked) so it runs on the
// default CPU build and exercises the shared RendererDriver feImage path.
// ----------------------------------------------------------------------------
TEST_F(GeodeTextDecorationRepro, FeImageFragmentRedrawIsIdempotent) {
  const std::filesystem::path resvgRoot =
      Runfiles::instance().RlocationExternal("resvg-test-suite", "");
  const char* svg = "donner/svg/renderer/testdata/feimage_fragment_idempotency.svg";

  SVGDocument document = loadSVG(svg, resvgRoot);

  // Two renders of the SAME document. With the leaked-shadow-instance bug, draw
  // #2's main snapshot includes the feImage fragment's offscreen shadow rect, so
  // it draws the green rect at (36,36) over the filtered output — differing from
  // draw #1.
  const RendererBitmap first = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  const RendererBitmap second = RenderDocumentWithBackend(document, RendererBackend::TinySkia);

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());

  // Strict identity: the two renders must be pixel-for-pixel identical.
  ExpectBitmapsIdentical(second, first, "feimage_fragment_redraw");
}

INSTANTIATE_TEST_SUITE_P(
    FiltersEnableBackground, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/enable-background",
                {
                    // Not impl: `enable-background` attribute + `in=BackgroundImage`/
                    // `in=BackgroundAlpha` filter inputs. Both were deprecated in
                    // SVG 2 and replaced by `<filter>` element chains and CSS
                    // `backdrop-filter`. Donner has never implemented them; the
                    // existing filter suite already skips the legacy test cases
                    // with the same rationale (see `in=BackgroundAlpha` / `=BackgroundImage`
                    // skips elsewhere in this file). See
                    // docs/unsupported_svg1_features.md.
                    // The 4 entries below (accumulate + new-with-invalid-region-{1,2,3})
                    // used to pass incidentally — their `in=BackgroundImage` produces
                    // empty output that matches the golden. But that empty-output path
                    // runs an empty compute dispatch against a zero-sized source on
                    // Geode, which hangs Mesa llvmpipe (CI) and Intel Arc Xe-KMD. Skip
                    // with the rest of the category until real `enable-background`
                    // plumbing lands (or is explicitly dropped — SVG 2 replaced it).
                    {"accumulate.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"accumulate-with-new.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"filter-on-shape.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"inherit.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"new-with-invalid-region-1.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"new-with-invalid-region-2.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"new-with-invalid-region-3.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"new-with-region.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"new.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"shapes-after-filter.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"stop-on-the-first-new-1.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"stop-on-the-first-new-2.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"with-clip-path.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"with-filter-on-the-same-element.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"with-filter.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"with-mask.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"with-opacity-1.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"with-opacity-2.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"with-opacity-3.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"with-opacity-4.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                    {"with-transform.svg",
                     Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeBlend, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory(
                                     "filters/feBlend",
                                     {
                                         {"with-subregion-on-input-1.svg",
                                          Params::Skip("Not impl: primitive subregion clipping")},
                                         {"with-subregion-on-input-2.svg",
                                          Params::Skip("Not impl: primitive subregion clipping")},
                                     })),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeColorMatrix, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/feColorMatrix",
            {
                {"type=hueRotate-without-an-angle.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "hueRotate(0) identity")},
                {"type=hueRotate.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "hueRotate(30)")},
                {"type=matrix-with-empty-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=matrix-with-non-normalized-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "non-normalized values")},
                {"type=matrix-with-not-enough-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=matrix-with-too-many-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=matrix-without-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=matrix.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "type=matrix")},
                {"type=saturate-with-a-large-coefficient.svg",
                 Params::RenderOnly("saturate 99999 (UB)")},
                {"type=saturate-with-negative-coefficient.svg",
                 Params::RenderOnly("saturate -0.5 (UB)")},
                {"type=saturate-without-a-coefficient.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity saturate")},
                {"type=saturate.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "saturate")},
                {"without-attributes.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "no attrs identity")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeComponentTransfer, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feComponentTransfer")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeComposite, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feComposite",
                {
                    {"default-operator.svg",
                     Params::Skip("Not impl: primitive subregion clipping (feFlood subregion)")},
                    {"invalid-operator.svg",
                     Params::Skip("Not impl: primitive subregion clipping (feFlood subregion)")},
                    {"operator=over.svg",
                     Params::Skip("Not impl: primitive subregion clipping (feFlood subregion)")},
                    {"with-subregion-on-input-1.svg",
                     Params::Skip("Not impl: primitive subregion clipping")},
                    {"with-subregion-on-input-2.svg",
                     Params::Skip("Not impl: primitive subregion clipping")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeConvolveMatrix, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feConvolveMatrix",
                {
                    {"bias=-0.5.svg", Params::RenderOnly("UB: bias=-0.5")},
                    {"bias=0.5.svg", Params::RenderOnly("UB: bias=0.5")},
                    {"bias=9999.svg", Params::RenderOnly("UB: bias=9999")},
                    {"edgeMode=wrap-with-matrix-larger-than-target.svg",
                     Params::RenderOnly("UB: wrap with oversized kernel")},
                    {"edgeMode=wrap.svg",
                     Params::WithThreshold(kDefaultThreshold, 200,
                                           "Minor algorithm differences on edge handling (180px)")},
                    {"kernelMatrix-with-zero-sum-and-no-divisor.svg",
                     Params::RenderOnly("MatrixConvolution edge shift vs golden")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeDiffuseLighting, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feDiffuseLighting",
                {
                    {"complex-transform.svg",
                     Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                           "Shading differences, donner is smoother")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeDisplacementMap, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feDisplacementMap")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeDistantLight, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feDistantLight")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeDropShadow, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feDropShadow",
                {
                    {"only-stdDeviation.svg",
                     Params::WithThreshold(0.04f, kDefaultMismatchedPixels, "Minor blur diffs")},
                    {"with-flood-color.svg",
                     Params::WithThreshold(0.03f, kDefaultMismatchedPixels, "Minor blur diffs")},

                    {"with-percent-offset.svg", Params::Skip("Bug: feDropShadow edge case")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeFlood, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feFlood")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeGaussianBlur, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feGaussianBlur",
                {
                    {"complex-transform.svg", Params::WithThreshold(0.03f, kDefaultMismatchedPixels,
                                                                    "Minor AA differences")},
                    {"huge-stdDeviation.svg",
                     Params::RenderOnly("Extreme sigma=1000; output is implementation-defined")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeImage, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/feImage",
            {
                {"chained-feImage.svg",
                 Params::WithThreshold(kDefaultThreshold, 22000, "Chained feImage fragment refs")},
                {"embedded-png.svg",
                 Params::WithThreshold(0.05f, 100,
                                       "Bilinear interpolation + sRGB↔linear roundtrip")},
                {"empty.svg", Params::Skip("Linux CI: std::bad_alloc in test setup.")},
                {"link-on-an-element-with-complex-transform.svg",
                 Params::WithThreshold(kDefaultThreshold, 26200,
                                       "Fragment ref with complex transform")},
                {"link-to-an-element-with-transform.svg",
                 Params::WithThreshold(kDefaultThreshold, 34200,
                                       "Fragment ref with skewX transform on element")},
                {"preserveAspectRatio=none.svg",
                 Params::WithThreshold(0.05f, 100,
                                       "Bilinear interpolation + sRGB↔linear roundtrip")},
                {"simple-case.svg", Params::Skip("External file reference (no ResourceLoader)")},
                {"svg.svg",
                 Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-svg.png")
                     .withReason("We render higher quality")},
                {"with-subregion-1.svg",
                 Params::WithThreshold(kDefaultThreshold, 5100, "OBB subregion bilinear")},
                {"with-subregion-2.svg",
                 Params::WithThreshold(kDefaultThreshold, 5100, "OBB subregion percentage")},
                {"with-subregion-3.svg",
                 Params::WithThreshold(kDefaultThreshold, 14500, "Percentage width subregion")},
                {"with-subregion-4.svg",
                 Params::WithThreshold(kDefaultThreshold, 15000, "Absolute subregion coords")},
                {"with-subregion-5.svg", Params::Skip("Subregion with rotation: filter")},

                {"with-x-y-and-protruding-subregion-1.svg",
                 Params::Skip("Bug: feImage edge cases / unsupported subregion combinations")},
                {"with-x-y-and-protruding-subregion-2.svg",
                 Params::Skip("Bug: feImage edge cases / unsupported subregion combinations")},
                {"with-x-y.svg",
                 Params::Skip("Bug: feImage edge cases / unsupported subregion combinations")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeMerge, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory(
                                     "filters/feMerge",
                                     {
                                         {"complex-transform.svg",
                                          Params::WithThreshold(0.15f, kDefaultMismatchedPixels,
                                                                "Minor blur shading differences")},
                                     })),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeMorphology, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/feMorphology",
            {
                {"empty-radius.svg",
                 Params::Skip("Bug: feMorphology edge cases (empty radius, non-numeric radius)")},
                {"negative-radius.svg",
                 Params::Skip("Bug: feMorphology edge cases (empty radius, non-numeric radius)")},
                {"no-radius.svg",
                 Params::Skip("Bug: feMorphology edge cases (empty radius, non-numeric radius)")},
                {"radius-with-too-many-values.svg",
                 Params::Skip("Bug: feMorphology edge cases (empty radius, non-numeric radius)")},
                {"zero-radius.svg",
                 Params::Skip("Bug: feMorphology edge cases (empty radius, non-numeric radius)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeOffset, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feOffset")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFePointLight, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("filters/fePointLight",
                                        {
                                            {"complex-transform.svg",
                                             Params::WithThreshold(0.1f, 120,
                                                                   "Minor shading differences")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeSpecularLighting, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feSpecularLighting",
                {
                    {"with-fePointLight.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-with-fePointLight.png", 0.02f)
                         .withReason("resvg golden")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeSpotLight, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feSpotLight",
                {
                    {"complex-transform.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-complex-transform.png")
                         .withReason("resvg bug: SpotLight Y")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeTile, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("filters/feTile",
                                        {
                                            {"complex-transform.svg",
                                             Params::RenderOnly("UB: complex transform")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeTurbulence, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feTurbulence",
                {
                    {"color-interpolation-filters=sRGB.svg",
                     Params::WithThreshold(0.05f, kDefaultMismatchedPixels,
                                           "Minor shading differences")},
                    {"complex-transform.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels,
                                                                    "Minor shading differences")},
                    {"stitchTiles=stitch.svg", Params::RenderOnly("UB: stitchTiles=stitch")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFilter, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/filter",
            {
                {"complex-order-and-xlink-href.svg",
                 Params::Skip("Bug: Color is slightly off, we are missing transparency")},
                {"in=BackgroundAlpha-with-enable-background.svg",
                 Params::Skip("in=BackgroundAlpha (deprecated SVG 1.1)")},
                {"in=BackgroundImage-with-enable-background.svg",
                 Params::Skip("in=BackgroundImage (deprecated SVG 1.1)")},
                {"in=FillPaint-on-g-without-children.svg",
                 Params::RenderOnly("UB: in=FillPaint on empty group")},
                {"in=FillPaint-with-gradient.svg", Params::RenderOnly("UB: in=FillPaint gradient")},
                {"in=FillPaint-with-pattern.svg", Params::RenderOnly("UB: in=FillPaint pattern")},
                {"in=FillPaint-with-target-on-g.svg",
                 Params::RenderOnly("UB: in=FillPaint on group")},
                {"in=FillPaint.svg", Params::RenderOnly("UB: in=FillPaint")},
                {"in=StrokePaint.svg", Params::RenderOnly("UB: in=StrokePaint")},
                {"on-the-root-svg.svg", Params::RenderOnly("UB: Filter on the root `svg`")},
                {"transform-on-shape-with-filter-region.svg",
                 Params::Skip("Bug: We don't blur the right edge")},
                {"with-subregion-3.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels,
                                                               "Minor shading differences")},

                {"content-outside-the-canvas-2.svg",
                 Params::Skip(
                     "Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
                {"in=BackgroundAlpha.svg",
                 Params::Skip(
                     "Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
                {"with-mask-on-parent.svg",
                 Params::Skip(
                     "Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
                {"with-transform-outside-of-canvas.svg",
                 Params::Skip(
                     "Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

// TODO: The entire filters/filter-functions category produces "Data corrupted"
// parse errors on CI x86_64 runners but passes locally on aarch64. The root
// cause is unknown — possibly a resvg test suite data integrity issue on CI,
// or an x86_64-specific parser bug. Disabled until investigated.
//
// INSTANTIATE_TEST_SUITE_P(
//     FiltersFilterFunctions, ImageComparisonTestFixture,
//     ValuesIn(getTestsInCategory("filters/filter-functions")),
//     TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFloodColor, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("filters/flood-color",
                                        {
                                            {"inheritance-3.svg",
                                             Params::Skip("230K diff: ICC color")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFloodOpacity, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/flood-opacity")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingClip, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("masking/clip",
                                        {
                                            {"simple-case.svg", Params::Skip()},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(MaskingClipRule, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("masking/clip-rule")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingClipPath, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "masking/clipPath",
            {
                {"clip-path-on-children.svg", Params::Skip("Bug: Nested clip-path not working")},
                {"clip-path-with-transform-on-text.svg",
                 Params::Skip("Not impl: clipPath on <text>")},
                {"clipping-with-complex-text-1.svg",
                 Params::Skip("Not impl: clipPath with <text> children")},
                {"clipping-with-complex-text-2.svg",
                 Params::Skip("Not impl: clipPath with <text> children")},
                {"clipping-with-complex-text-and-clip-rule.svg",
                 Params::Skip("Not impl: clipPath with <text> children")},
                {"clipping-with-text.svg", Params::Skip("Not impl: clipPath with <text> children")},
                {"on-the-root-svg-without-size.svg",
                 Params::RenderOnly("UB: on root `<svg>` without size")},
                {"switch-is-not-a-valid-child.svg", Params::Skip("Not impl: <switch>")},
                {"with-use-child.svg", Params::Skip("Not impl: <use> child")},

                {"circle-shorthand-with-stroke-box.svg",
                 Params::Skip("Bug: clipPath edge cases beyond core support")},
                {"circle-shorthand-with-view-box.svg",
                 Params::Skip("Bug: clipPath edge cases beyond core support")},
                {"circle-shorthand.svg",
                 Params::Skip("Bug: clipPath edge cases beyond core support")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingMask, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "masking/mask",
                {
                    {"color-interpolation=linearRGB.svg",
                     Params::Skip("Not implemented: color-interpolation linearRGB")},
                    {"mask-on-self.svg",
                     Params::Skip("Non-text mask regression kept out of text stack")},
                    {"recursive-on-child.svg", Params::RenderOnly("UB: Recursive on child")},
                    {"with-image.svg",
                     Params::WithThreshold(0.1f, kDefaultMismatchedPixels,
                                           "Mask with <image> (bilinear edge diffs)")},

                    {"half-width-region-with-rotation.svg",
                     Params::Skip(
                         "Bug: mask edge cases (color-interpolation, mask-units, mask-type) need "
                         "investigation")},
                    {"mask-on-self-with-mask-type=alpha.svg",
                     Params::Skip(
                         "Bug: mask edge cases (color-interpolation, mask-units, mask-type) need "
                         "investigation")},
                    {"mask-on-self-with-mixed-mask-type.svg",
                     Params::Skip(
                         "Bug: mask edge cases (color-interpolation, mask-units, mask-type) need "
                         "investigation")},
                    {"mask-type-in-style.svg",
                     Params::Skip("Bug: mask edge cases (color-interpolation, "
                                  "mask-units, mask-type) need investigation")},
                    {"mask-type=alpha.svg",
                     Params::Skip("Bug: mask edge cases (color-interpolation, "
                                  "mask-units, mask-type) need investigation")},
                    {"on-group-with-transform.svg",
                     Params::Skip(
                         "Bug: mask edge cases (color-interpolation, mask-units, mask-type) need "
                         "investigation")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersLinearGradient, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "paint-servers/linearGradient",
                {
                    {"invalid-gradientTransform.svg",
                     Params::RenderOnly("UB: Invalid `gradientTransform`")},

                    {"gradientUnits=userSpaceOnUse-with-percent.svg",
                     Params::Skip("Bug: intrinsic sizing + percent resolution with "
                                  "non-square viewBox; see ShapesEllipse")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersPattern, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "paint-servers/pattern",
                {
                    {"invalid-patternTransform.svg",
                     Params::RenderOnly("UB: Invalid patternTransform")},
                    {"out-of-order-referencing.svg",
                     Params::WithThreshold(0.6f, 800, "Nested pattern AA (768px)")},
                    {"overflow=visible.svg", Params::RenderOnly("UB: overflow=visible")},
                    {"pattern-on-child.svg", Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                                                   "Anti-aliasing artifacts")},
                    {"patternContentUnits-with-viewBox.svg",
                     Params::WithThreshold(kDefaultThreshold, 150, "Pattern AA drift")},
                    {"patternContentUnits=objectBoundingBox.svg",
                     Params::WithThreshold(kDefaultThreshold, 250, "Pattern AA drift")},
                    {"recursive-on-child.svg",
                     Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                           "Larger threshold due to recursive pattern seams.")},
                    {"self-recursive-on-child.svg",
                     Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                           "Larger threshold due to recursive pattern seams.")},
                    {"self-recursive.svg",
                     Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                           "Larger threshold due to recursive pattern seams.")},
                    {"text-child.svg",
                     Params::WithThreshold(0.5f, 1150, "AA artifacts + quad glyph outlines")},
                    {"tiny-pattern-upscaled.svg",
                     Params::WithThreshold(0.02f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersRadialGradient, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "paint-servers/radialGradient",
                {
                    {"focal-point-correction.svg",
                     Params::Skip("Test suite bug? In SVG2 this was changed to draw")},
                    {"fr=-1.svg", Params::RenderOnly("UB: fr=-1 (SVG 2)")},
                    {"fr=0.5.svg", Params::RenderOnly("UB: fr=0.5 (SVG 2)")},
                    {"fr=0.7.svg", Params::Skip("Test suite bug? fr > default value of")},
                    {"invalid-gradientTransform.svg",
                     Params::RenderOnly("UB: Invalid `gradientTransform`")},
                    {"invalid-gradientUnits.svg",
                     Params::RenderOnly("UB: Invalid `gradientUnits`")},
                    {"negative-r.svg", Params::RenderOnly("UB: Negative `r`")},

                    {"gradientUnits=objectBoundingBox-with-percent.svg",
                     Params::Skip(
                         "Bug: intrinsic sizing + percent resolution with non-square viewBox; see "
                         "ShapesEllipse")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersStop, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("paint-servers/stop",
                                        {
                                            {"stop-color-with-inherit-1.svg",
                                             Params::Skip("Bug? Strange edge case, stop-color")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintServersStopColor, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("paint-servers/stop-color")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintServersStopOpacity, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("paint-servers/stop-opacity")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingColor, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/color")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingContext, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "painting/context",
            {
                {"in-marker.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                {"in-nested-marker.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                {"in-nested-use-and-marker.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                {"on-shape-with-zero-size-bbox.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                {"with-gradient-and-gradient-transform.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                {"with-gradient-in-use.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                {"with-gradient-on-marker.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                {"with-pattern-and-transform-in-use.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                {"with-pattern-in-use.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                {"with-pattern-objectBoundingBox-in-use.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                {"with-pattern-on-marker.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                {"with-text.svg",
                 Params::Skip(
                     "Not impl: context-fill / context-stroke (parsed but not honored at render)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingDisplay, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("painting/display",
                                        {
                                            {"none-on-tref.svg", Params::Skip("Not impl: <tref>")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingFill, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/fill",
                {
                    {"icc-color.svg", Params::RenderOnly("UB: ICC color")},
                    {"linear-gradient-on-text.svg", Params::WithThreshold(kDefaultThreshold, 500)},
                    {"pattern-on-text.svg", Params::WithThreshold(kDefaultThreshold, 2100)},
                    {"radial-gradient-on-text.svg", Params::WithThreshold(kDefaultThreshold, 500)},
                    {"rgb-int-int-int.svg", Params::RenderOnly("UB: rgb(int int int)")},
                    {"valid-FuncIRI-with-a-fallback-ICC-color.svg",
                     Params::Skip("Not impl: Fallback with icc-color")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingFillOpacity, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/fill-opacity")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingFillRule, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/fill-rule")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingImageRendering, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "painting/image-rendering",
            {
                {"on-feImage.svg",
                 Params::Skip("Not impl: image-rendering property (pixelated/crisp-edges/smooth)")},
                {"optimizeSpeed.svg",
                 Params::Skip("Not impl: image-rendering property (pixelated/crisp-edges/smooth)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingIsolation, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/isolation")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingMarker, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "painting/marker",
            {
                {"marker-on-text.svg", Params::Skip("Not impl: `text`")},
                {"orient=auto-on-M-C-C-4.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-orient=auto-on-M-C-C-4.png")
                     .withReason("Pre-existing rendering diff (stroke/AA), not cusp-related")},
                {"orient=auto-on-M-L-L-Z-Z-Z.svg", Params::Skip("Bug: Multiple closepaths")},
                {"target-with-subpaths-2.svg", Params::RenderOnly("UB: Target with subpaths")},
                {"with-a-text-child.svg",
                 Params::WithThreshold(kDefaultThreshold, 110, "Minor AA diffs on text_full")},
                {"with-an-image-child.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-an-image-child.png")
                     .withReason("We (correctly)")},
                {"with-viewBox-1.svg", Params::RenderOnly("UB: with `viewBox`")},

                {"marker-on-rounded-rect.svg",
                 Params::Skip("Bug: marker edge cases (rounded-rect path corners, recursive-5)")},
                {"percent-values.svg",
                 Params::Skip("Bug: intrinsic sizing + percent resolution with "
                              "non-square viewBox; see ShapesEllipse")},
                {"recursive-5.svg",
                 Params::Skip("Bug: marker edge cases (rounded-rect path corners, recursive-5)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingMixBlendMode, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/mix-blend-mode")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingOpacity, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/opacity",
                {
                    {"50percent.svg",
                     Params::Skip("Changed in css-color-4 to allow percentage in")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingOverflow, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/overflow")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingPaintOrder, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "painting/paint-order",
            {
                {"fill-markers-stroke.svg",
                 Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                {"markers-stroke.svg",
                 Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                {"markers.svg",
                 Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                {"on-text.svg",
                 Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                {"on-tspan.svg",
                 Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                {"stroke-markers-fill.svg",
                 Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                {"stroke-markers.svg",
                 Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                {"stroke.svg",
                 Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingShapeRendering, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/shape-rendering")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStroke, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/stroke",
                {
                    {"linear-gradient-on-text.svg",
                     Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "AA artifacts")},
                    {"pattern-on-text.svg",
                     Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "AA artifacts")},
                    {"radial-gradient-on-text.svg", Params::Skip("Bug: Gradient stroke on text")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStrokeDasharray, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/stroke-dasharray",
                {
                    {"multiple-subpaths.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Larger threshold due to anti-aliasing artifacts.")},
                    {"negative-sum.svg", Params::RenderOnly("UB (negative sum)")},
                    {"negative-values.svg", Params::RenderOnly("UB (negative values)")},

                    {"0-n-with-butt-caps.svg",
                     Params::Skip("Bug: stroke-dasharray edge cases (specific value patterns)")},
                    {"0-n-with-round-caps.svg",
                     Params::Skip("Bug: stroke-dasharray edge cases (specific value patterns)")},
                    {"0-n-with-square-caps.svg",
                     Params::Skip("Bug: stroke-dasharray edge cases (specific value patterns)")},
                    {"n-0.svg",
                     Params::Skip("Bug: stroke-dasharray edge cases (specific value patterns)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeDashoffset, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/stroke-dashoffset")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeLinecap, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/stroke-linecap")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStrokeLinejoin, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/stroke-linejoin",
                {
                    {"arcs.svg", Params::RenderOnly("UB (SVG 2), no UA supports `arcs`")},
                    {"miter-clip.svg",
                     Params::RenderOnly("UB (SVG 2), no UA supports `miter-clip`")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeMiterlimit, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/stroke-miterlimit")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeOpacity, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/stroke-opacity")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStrokeWidth, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("painting/stroke-width",
                                        {
                                            {"negative.svg",
                                             Params::RenderOnly("UB: Nothing should be rendered")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingVisibility, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/visibility",
                {
                    {"bbox-impact-3.svg",
                     Params::Skip("Not impl: <text> contributing to bbox handling")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesCircle, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("shapes/circle")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    ShapesEllipse, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "shapes/ellipse",
                {
                    // Bug: SVG has viewBox="0 0 200 100" with no width/height. Donner's
                    // intrinsic document sizing computes 500x375 (wrong) instead of
                    // 500x250, and percent-valued geometry (cx=50%, ry=20%, ...) then
                    // resolves against that mis-sized viewport, so the ellipse is
                    // larger than the golden and shifted. Root cause traced to
                    // LayoutSystem::calculateRawDocumentSize's use of transformPosition
                    // (which folds in the aspect-ratio letterbox translation) instead
                    // of transformVector; fixing it in isolation breaks the percent
                    // resolution pipeline downstream, so deferring to a dedicated PR.
                    {"percent-values-missing-ry.svg",
                     Params::Skip("Bug: intrinsic sizing + percent resolution with "
                                  "non-square viewBox")},
                    {"percent-values.svg",
                     Params::Skip("Bug: intrinsic sizing + percent resolution with "
                                  "non-square viewBox")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    ShapesLine, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "shapes/line",
                {
                    {"simple-case.svg",
                     Params::WithThreshold(0.02f, kDefaultMismatchedPixels,
                                           "Larger threshold due to anti-aliasing")},
                    // Bug: see ShapesEllipse — non-square viewBox + percent geometry.
                    {"percent-units.svg",
                     Params::Skip("Bug: intrinsic sizing + percent resolution with "
                                  "non-square viewBox")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesPath, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("shapes/path")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesPolygon, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("shapes/polygon")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesPolyline, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("shapes/polyline")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    ShapesRect, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "shapes/rect",
                {
                    // Bug: see ShapesEllipse — non-square viewBox + percent geometry.
                    {"percentage-values-1.svg",
                     Params::Skip("Bug: intrinsic sizing + percent resolution with "
                                  "non-square viewBox")},
                    {"percentage-values-2.svg",
                     Params::Skip("Bug: intrinsic sizing + percent resolution with "
                                  "non-square viewBox")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureA, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/a",
                {
                    {"inside-text.svg",
                     Params::Skip("Not impl: <a> link element rendering / hyperlink processing")},
                    {"inside-tspan.svg",
                     Params::Skip("Not impl: <a> link element rendering / hyperlink processing")},
                    {"on-tspan.svg",
                     Params::Skip("Not impl: <a> link element rendering / hyperlink processing")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureDefs, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("structure/defs",
                                        {
                                            {"style-inheritance-on-text.svg",
                                             Params::WithThreshold(kDefaultThreshold, 6500)},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureG, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("structure/g")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureImage, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "structure/image",
            {
                {"float-size.svg", Params::RenderOnly("UB: Float size")},
                {"no-height-on-svg.svg", Params::RenderOnly("UB: No height")},
                {"no-width-and-height-on-svg.svg", Params::RenderOnly("UB: No width and height")},
                {"no-width-on-svg.svg", Params::RenderOnly("UB: No width")},
                {"url-to-png.svg", Params::Skip("Not impl: External URLs")},
                {"url-to-svg.svg", Params::Skip("Not impl: External URLs")},

                {"embedded-16bit-png.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"embedded-gif.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"embedded-jpeg-without-mime.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"embedded-png.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"embedded-svg-with-text.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"external-gif.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"external-png.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"no-height-non-square.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"no-height.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"no-width-and-height.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"no-width.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"preserveAspectRatio=none.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"preserveAspectRatio=xMaxYMax-meet.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"preserveAspectRatio=xMidYMid-meet.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"preserveAspectRatio=xMinYMin-meet.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"raster-image-and-size-with-odd-numbers.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"width-and-height-set-to-auto.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
                {"with-transform.svg",
                 Params::Skip(
                     "Bug: <image> rendering layout/sizing differs from golden (embedded data URLs "
                     "render but at wrong size; preserveAspectRatio modes need investigation)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureStyle, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("structure/style",
                                        {
                                            {"external-CSS.svg",
                                             Params::Skip("Not impl: CSS @import")},
                                            {"non-presentational-attribute.svg",
                                             Params::Skip("Not impl: <svg version=\"1.1\">")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureStyleAttribute, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/style-attribute",
                {
                    {"non-presentational-attribute.svg",
                     Params::Skip("<svg version=\"1.1\"> disables geometry attributes in style")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureSvg, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/svg",
                {
                    {"attribute-value-via-ENTITY-reference.svg",
                     Params::Skip("Bug/Not impl? XML Entity references")},
                    {"elements-via-ENTITY-reference-2.svg",
                     Params::Skip("Bug/Not impl? XML Entity references")},
                    {"elements-via-ENTITY-reference-3.svg",
                     Params::Skip("Bug/Not impl? XML Entity references")},
                    {"funcIRI-parsing.svg", Params::RenderOnly("UB: FuncIRI parsing")},
                    {"funcIRI-with-invalid-characters.svg",
                     Params::RenderOnly("UB: FuncIRI with invalid chars")},
                    {"invalid-id-attribute-1.svg", Params::RenderOnly("UB: Invalid id attribute")},
                    {"invalid-id-attribute-2.svg", Params::RenderOnly("UB: Invalid id attribute")},
                    {"mixed-namespaces.svg", Params::Skip("Bug? mixed namespaces")},
                    {"nested-svg-with-overflow-auto.svg", Params::Skip("Not impl: overflow")},
                    {"nested-svg-with-overflow-visible.svg", Params::Skip("Not impl: overflow")},
                    {"no-size.svg", Params::Skip("Not impl: Computed bounds from content")},
                    {"not-UTF-8-encoding.svg", Params::Skip("Bug/Not impl? Non-UTF8 encoding")},
                    {"preserveAspectRatio-with-viewBox-not-at-zero-pos.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"preserveAspectRatio=none.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"preserveAspectRatio=xMaxYMax-slice.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"preserveAspectRatio=xMaxYMax.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"preserveAspectRatio=xMidYMid-slice.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"preserveAspectRatio=xMidYMid.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"preserveAspectRatio=xMinYMin-slice.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"preserveAspectRatio=xMinYMin.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"proportional-viewBox.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"rect-inside-a-non-SVG-element.svg",
                     Params::Skip("Bug? Rect inside unknown element")},
                    {"viewBox-not-at-zero-pos.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"xmlns-validation.svg", Params::Skip("Bug? xmlns validation")},

                    {"funcIRI-with-quotes.svg", Params::Skip("Bug: <svg> root element edge cases")},
                    {"nested-svg-one-with-rect-and-one-with-viewBox.svg",
                     Params::Skip("Bug: <svg> root element edge cases")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureSwitch, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/switch",
                {
                    {"comment-as-first-child.svg", Params::Skip("Not impl: <switch>")},
                    {"display-none-on-child.svg", Params::Skip("Not impl: <switch>")},
                    {"non-SVG-child.svg", Params::Skip("Not impl: <switch>")},
                    {"requiredFeatures.svg", Params::Skip("Not impl: <switch>")},
                    {"simple-case.svg", Params::Skip("Not impl: <switch>")},
                    {"systemLanguage.svg", Params::Skip("Not impl: <switch>")},
                    {"systemLanguage=en-GB.svg", Params::Skip("Not impl: <switch>")},
                    {"systemLanguage=en-US.svg", Params::Skip("Not impl: <switch>")},
                    {"systemLanguage=en.svg", Params::Skip("Not impl: <switch>")},
                    {"systemLanguage=ru-Ru.svg", Params::Skip("Not impl: <switch>")},
                    {"systemLanguage=ru-en.svg", Params::Skip("Not impl: <switch>")},
                    {"with-attributes.svg", Params::Skip("Not impl: <switch>")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureSymbol, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("structure/symbol",
                                        {
                                            {"with-transform.svg",
                                             Params::Skip("New SVG2 feature, transform on symbol")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureSystemLanguage, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/systemLanguage",
                {
                    {"on-svg.svg", Params::Skip("Not impl: systemLanguage conditional processing")},
                    {"on-tspan.svg",
                     Params::Skip("Not impl: systemLanguage conditional processing")},
                    {"ru-Ru.svg", Params::Skip("Not impl: systemLanguage conditional processing")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureTransform, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/transform",
                {
                    {"rotate-at-position.svg",
                     Params::WithThreshold(0.05f, kDefaultMismatchedPixels,
                                           "Larger threshold due to anti-aliasing artifacts.")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureTransformOrigin, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/transform-origin",
                {
                    // TODO(#514): transform-origin rendering is broken after #514's
                    // single-keyword parsing changes — 10K-150K pixel diffs indicate
                    // the rendering is completely wrong, not just slightly off. Disabled
                    // until the root cause is investigated and fixed properly.
                    {"bottom.svg", Params::Skip("transform-origin broken after #514")},
                    {"center.svg", Params::Skip("transform-origin broken after #514")},
                    {"keyword-length.svg", Params::Skip("transform-origin broken after #514")},
                    {"left.svg", Params::Skip("transform-origin broken after #514")},
                    {"length-percent.svg", Params::Skip("transform-origin broken after #514")},
                    {"length-px.svg", Params::Skip("transform-origin broken after #514")},
                    {"on-clippath-objectBoundingBox.svg",
                     Params::Skip("transform-origin broken after #514")},
                    {"on-clippath.svg", Params::Skip("transform-origin broken after #514")},
                    {"on-gradient-object-bounding-box.svg",
                     Params::Skip("transform-origin broken after #514")},
                    {"on-gradient-user-space-on-use.svg",
                     Params::Skip("transform-origin broken after #514")},
                    {"on-group.svg", Params::Skip("transform-origin broken after #514")},
                    {"on-image.svg", Params::Skip("transform-origin broken after #514")},
                    {"on-pattern-object-bounding-box.svg",
                     Params::Skip("transform-origin broken after #514")},
                    {"on-pattern-user-space-on-use.svg",
                     Params::Skip("transform-origin broken after #514")},
                    {"on-shape.svg", Params::Skip("transform-origin broken after #514")},
                    {"on-text-path.svg", Params::Skip("transform-origin broken after #514")},
                    {"on-text.svg", Params::Skip("transform-origin broken after #514")},
                    {"right-bottom.svg", Params::Skip("transform-origin broken after #514")},
                    {"right.svg", Params::Skip("transform-origin broken after #514")},
                    {"top.svg", Params::Skip("transform-origin broken after #514")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureUse, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/use",
                {
                    {"xlink-to-an-external-file.svg", Params::Skip("Not impl: External file.")},

                    {"nested-xlink-to-svg-element-with-rect-and-size.svg",
                     Params::Skip("Bug: <use> referencing inline <svg> elements with "
                                  "various width/height/viewBox combinations")},
                    {"xlink-to-svg-element-with-rect-only-width.svg",
                     Params::Skip("Bug: <use> referencing inline <svg> elements with "
                                  "various width/height/viewBox combinations")},
                    {"xlink-to-svg-element-with-rect.svg",
                     Params::Skip("Bug: <use> referencing inline <svg> elements with "
                                  "various width/height/viewBox combinations")},
                    {"xlink-to-svg-element-with-viewBox.svg",
                     Params::Skip("Bug: <use> referencing inline <svg> elements with "
                                  "various width/height/viewBox combinations")},
                    {"xlink-to-svg-element-with-width-height-on-use.svg",
                     Params::Skip("Bug: <use> referencing inline <svg> elements with "
                                  "various width/height/viewBox combinations")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextAlignmentBaseline, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/alignment-baseline",
            {
                {"after-edge.svg",
                 Params::Skip(
                     "Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                {"baseline.svg",
                 Params::Skip(
                     "Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                {"before-edge.svg",
                 Params::Skip(
                     "Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                {"hanging-on-vertical.svg",
                 Params::Skip(
                     "Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                {"ideographic.svg",
                 Params::Skip(
                     "Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                {"middle-on-textPath.svg",
                 Params::Skip(
                     "Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                {"middle.svg",
                 Params::Skip(
                     "Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                {"text-after-edge.svg",
                 Params::Skip(
                     "Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                {"text-before-edge.svg",
                 Params::Skip(
                     "Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                {"two-textPath-with-middle-on-first.svg",
                 Params::Skip(
                     "Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextBaselineShift, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("text/baseline-shift",
                                        {
                                            {"nested-with-baseline-1.svg",
                                             Params::WithThreshold(0.1f, kDefaultMismatchedPixels,
                                                                   "Minor AA artifacts on axis")},
                                            {"nested-with-baseline-2.svg",
                                             Params::WithThreshold(0.1f, kDefaultMismatchedPixels,
                                                                   "Minor AA artifacts on axis")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextDirection, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/direction",
                {
                    {"rtl-with-vertical-writing-mode.svg",
                     Params::Skip("Not impl: direction property (BiDi text shaping)")},
                    {"rtl.svg", Params::Skip("Not impl: direction property (BiDi text shaping)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextDominantBaseline, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/dominant-baseline",
                {
                    {"alignment-baseline-and-baseline-shift-on-tspans.svg",
                     Params::Skip(
                         "Not impl: full dominant-baseline keyword set (incl. before/after-edge, "
                         "no-change, reset-size, use-script)")},
                    {"alignment-baseline=baseline-on-tspan.svg",
                     Params::Skip(
                         "Not impl: full dominant-baseline keyword set (incl. before/after-edge, "
                         "no-change, reset-size, use-script)")},
                    {"complex.svg",
                     Params::Skip("Not impl: full dominant-baseline keyword set (incl. "
                                  "before/after-edge, no-change, reset-size, use-script)")},
                    {"dummy-tspan.svg",
                     Params::Skip(
                         "Not impl: full dominant-baseline keyword set (incl. before/after-edge, "
                         "no-change, reset-size, use-script)")},
                    {"hanging.svg",
                     Params::Skip("Not impl: full dominant-baseline keyword set (incl. "
                                  "before/after-edge, no-change, reset-size, use-script)")},
                    {"inherit.svg",
                     Params::Skip("Not impl: full dominant-baseline keyword set (incl. "
                                  "before/after-edge, no-change, reset-size, use-script)")},
                    {"middle.svg",
                     Params::Skip("Not impl: full dominant-baseline keyword set (incl. "
                                  "before/after-edge, no-change, reset-size, use-script)")},
                    {"nested.svg",
                     Params::Skip("Not impl: full dominant-baseline keyword set (incl. "
                                  "before/after-edge, no-change, reset-size, use-script)")},
                    {"no-change.svg",
                     Params::Skip(
                         "Not impl: full dominant-baseline keyword set (incl. before/after-edge, "
                         "no-change, reset-size, use-script)")},
                    {"reset-size.svg",
                     Params::Skip(
                         "Not impl: full dominant-baseline keyword set (incl. before/after-edge, "
                         "no-change, reset-size, use-script)")},
                    {"sequential.svg",
                     Params::Skip(
                         "Not impl: full dominant-baseline keyword set (incl. before/after-edge, "
                         "no-change, reset-size, use-script)")},
                    {"text-after-edge.svg",
                     Params::Skip(
                         "Not impl: full dominant-baseline keyword set (incl. before/after-edge, "
                         "no-change, reset-size, use-script)")},
                    {"text-before-edge.svg",
                     Params::Skip(
                         "Not impl: full dominant-baseline keyword set (incl. before/after-edge, "
                         "no-change, reset-size, use-script)")},
                    {"use-script.svg",
                     Params::Skip(
                         "Not impl: full dominant-baseline keyword set (incl. before/after-edge, "
                         "no-change, reset-size, use-script)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFont, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/font",
                {
                    {"simple-case.svg", Params::Skip("Canvas size mismatch (400 vs 500)")},

                    {"font-shorthand.svg", Params::Skip("Not impl: font shorthand property")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontFamily, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/font-family",
            {
                {"bold-sans-serif.svg", Params::WithThreshold(kDefaultThreshold, 5200,
                                                              "Bold sans-serif (Noto Sans Bold)")},
                {"cursive.svg",
                 Params::WithThreshold(kDefaultThreshold, 5000, "cursive (Yellowtail)")},
                {"fallback-1.svg", Params::Skip("Fallback from invalid family (different")},
                {"fallback-2.svg", Params::WithThreshold(kDefaultThreshold, 1000,
                                                         "Fallback list: \"Invalid, Noto Sans\"")},
                {"fantasy.svg",
                 Params::WithThreshold(kDefaultThreshold, 5200, "fantasy (Sedgwick Ave Display)")},
                {"font-list.svg", Params::WithThreshold(kDefaultThreshold, 1300,
                                                        "Font list: Source Sans Pro fallback")},
                {"monospace.svg",
                 Params::WithThreshold(kDefaultThreshold, 600, "monospace (Noto Mono)")},
                {"sans-serif.svg",
                 Params::WithThreshold(kDefaultThreshold, 1900, "sans-serif (Noto Sans)")},
                {"serif.svg", Params::WithThreshold(kDefaultThreshold, 4200, "serif (Noto Serif)")},
                {"source-sans-pro.svg",
                 Params::WithThreshold(kDefaultThreshold, 1300, "Source Sans Pro")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontKerning, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/font-kerning",
                {
                    {"arabic-script.svg",
                     Params::Skip("Not impl: font-kerning property (HarfBuzz feature toggle)")},
                    {"none.svg",
                     Params::Skip("Not impl: font-kerning property (HarfBuzz feature toggle)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontSize, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/font-size",
            {
                {"named-value-without-a-parent.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-named-value-without-a-parent.png")
                     .withReason("Donner uses CSS Fonts Level 4, which has")},
                {"named-value.svg", Params::WithGoldenOverride(
                                        "donner/svg/renderer/testdata/golden/resvg-named-value.png")
                                        .withReason("Donner uses CSS Fonts Level 4, which has")},
                {"negative-size.svg", Params::RenderOnly("UB: negative font size")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontSizeAdjust, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("text/font-size-adjust",
                                        {
                                            {"simple-case.svg",
                                             Params::Skip("Not impl: font-size-adjust property")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextFontStretch, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("text/font-stretch")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextFontStyle, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("text/font-style")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontVariant, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/font-variant",
                {
                    {"inherit.svg", Params().withSimpleTextMaxPixels(1200).withReason(
                                        "small-caps is emulated with simple text")},
                    {"small-caps.svg", Params().withSimpleTextMaxPixels(1200).withReason(
                                           "small-caps is emulated with simple text")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextFontWeight, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("text/font-weight")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextGlyphOrientationHorizontal, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/glyph-orientation-horizontal",
                {
                    {"simple-case.svg",
                     Params::Skip("Not impl: glyph-orientation-horizontal (deprecated SVG 1.1)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextGlyphOrientationVertical, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/glyph-orientation-vertical",
                {
                    {"simple-case.svg",
                     Params::Skip("Not impl: glyph-orientation-vertical (deprecated SVG 1.1)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextKerning, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/kerning",
                {
                    {"0.svg", Params::Skip("Not impl: kerning attribute (deprecated SVG 1.1)")},
                    {"10percent.svg",
                     Params::Skip("Not impl: kerning attribute (deprecated SVG 1.1)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextLengthAdjust, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/lengthAdjust",
                {
                    {"text-on-path.svg",
                     Params::Skip("Not impl: lengthAdjust attribute (parented to textLength)")},
                    {"vertical.svg",
                     Params::Skip("Not impl: lengthAdjust attribute (parented to textLength)")},
                    {"with-underline.svg",
                     Params::Skip("Not impl: lengthAdjust attribute (parented to textLength)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextLetterSpacing, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/letter-spacing",
                {
                    {"large-negative.svg", Params::RenderOnly("UB: negative letter-spacing")},
                    {"mixed-scripts.svg",
                     Params::Skip("Needs BiDi: mixed LTR Latin + RTL Arabic in one span")},
                    {"non-ASCII-character.svg",
                     Params::Skip("Bug? We render with a different CJK glyph. Wrong font?")},
                    {"on-Arabic.svg", Params()
                                          .requireFeature(RendererBackendFeature::TextFull)
                                          .withReason("Arabic text")},

                    {"filter-bbox.svg", Params::Skip("Bug: letter-spacing edge cases")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextText, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/text",
                {
                    {"bidi-reordering.svg", Params::Skip("Not impl: Bidirectional text shaping")},
                    {"complex-grapheme-split-by-tspan.svg",
                     Params::RenderOnly("UB: grapheme split by tspan")},
                    {"complex-graphemes-and-coordinates-list.svg",
                     Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                                "resvg-complex-graphemes-and-coordinates-list.png")
                         .onlyTextFull()
                         .withReason("Simple text can't compose combining marks")},
                    {"complex-graphemes.svg",
                     Params().onlyTextFull().withReason("Combining mark needs HarfBuzz")},
                    {"compound-emojis-and-coordinates-list.svg",
                     Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                                "resvg-compound-emojis-and-coordinates-list.png",
                                                0.1f)
                         .withMaxPixelsDifferent(1100)
                         .onlyTextFull()
                         .withReason("Emoji bitmap scaling differs from the golden")},
                    {"compound-emojis.svg",
                     Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                           "Emoji, differences between resvg and our bitmap")
                         .onlyTextFull()},
                    {"emojis.svg", Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                                         "Emoji, differences between")
                                       .onlyTextFull()},
                    {"fill-rule=evenodd.svg",
                     Params().onlyTextFull().withReason("Arabic text shaping requires text-full")},
                    {"rotate-on-Arabic.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-rotate-on-Arabic.png")
                         .onlyTextFull()
                         .withReason("Arabic text shaping requires text-full,")},
                    {"rotate-with-multiple-values-and-complex-text.svg",
                     Params().onlyTextFull().withReason("Complex diatrics requires text-full")},
                    {"x-and-y-with-multiple-values-and-arabic-text.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/"
                         "resvg-x-and-y-with-multiple-values-and-arabic-text.png")
                         .withMaxPixelsDifferent(400)
                         .onlyTextFull()
                         .withReason("Arabic text shaping; vertical-axis AA diff not "
                                     "the focus of the test")},
                    {"xml-lang=ja.svg", Params::WithThreshold(kDefaultThreshold, 19100)},
                    {"xml-space.svg", Params::WithThreshold(kDefaultThreshold, 1400)},
                    {"zalgo.svg", Params().withMaxPixelsDifferent(300).onlyTextFull().withReason(
                                      "Complex diacritics; vertical-axis AA diff "
                                      "not the focus of the test")},

                    {"filter-bbox.svg",
                     Params::Skip(
                         "Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                    {"ligatures-handling-in-mixed-fonts-1.svg",
                     Params::Skip(
                         "Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                    {"ligatures-handling-in-mixed-fonts-2.svg",
                     Params::Skip(
                         "Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                    {"percent-value-on-dx-and-dy.svg",
                     Params::Skip(
                         "Bug: intrinsic sizing + percent resolution with non-square viewBox; see "
                         "ShapesEllipse")},
                    {"percent-value-on-x-and-y.svg",
                     Params::Skip(
                         "Bug: intrinsic sizing + percent resolution with non-square viewBox; see "
                         "ShapesEllipse")},
                    {"real-text-height.svg",
                     Params::Skip(
                         "Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextAnchor, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/text-anchor",
                {
                    {"coordinates-list.svg",
                     Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Axis AA artifacts")},
                    {"on-tspan-with-arabic.svg",
                     Params().requireFeature(RendererBackendFeature::TextFull)},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextDecoration, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/text-decoration",
            {
                {"all-types-inline-comma-separated.svg",
                 Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                {"all-types-inline-no-spaces.svg",
                 Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                {"all-types-inline.svg",
                 Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                {"indirect.svg", Params::WithGoldenOverride(
                                     "donner/svg/renderer/testdata/golden/resvg-indirect.png")},
                {"tspan-decoration.svg",
                 Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                {"underline-with-rotate-list-4.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "Minor shading diffs")},

                {"indirect-with-multiple-colors.svg",
                 Params::Skip(
                     "Not impl: text-decoration full SVG2 support (line/style/color independent)")},
                {"with-textLength-on-a-single-character.svg",
                 Params::Skip(
                     "Not impl: text-decoration full SVG2 support (line/style/color independent)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextTextRendering, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("text/text-rendering")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextLength, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/textLength",
                {
                    {"on-text-and-tspan.svg",
                     Params::Skip("Bug? We compress slightly more than the golden")},

                    {"arabic-with-lengthAdjust.svg",
                     Params::Skip("Not impl: textLength + lengthAdjust attribute (text "
                                  "stretching/compressing)")},
                    {"arabic.svg", Params::Skip("Not impl: textLength + lengthAdjust attribute "
                                                "(text stretching/compressing)")},
                    {"on-a-single-tspan.svg",
                     Params::Skip("Not impl: textLength + lengthAdjust attribute (text "
                                  "stretching/compressing)")},
                    {"zero.svg", Params::Skip("Not impl: textLength + lengthAdjust attribute (text "
                                              "stretching/compressing)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextPath, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/textPath",
            {
                {"closed-path.svg", Params::WithThreshold(0.1f, 400, "Minor AA diffs")},
                {"complex.svg", Params::Skip("Deferred: vertical + circular path")},
                {"dy-with-tiny-coordinates.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-dy-with-tiny-coordinates.png",
                     0.05f)
                     .withMaxPixelsDifferent(1100)
                     .withReason(
                         "AA + minor char advance diffs, different w/ text vs. text-full so")},
                {"link-to-rect.svg", Params::Skip("Not impl: link to rect (SVG 2)")},
                {"m-A-path.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "AA artifacts")},
                {"m-L-Z-path.svg", Params::WithGoldenOverride(
                                       "donner/svg/renderer/testdata/golden/resvg-m-L-Z-path.png")
                                       .withReason("Minor char")},
                {"method=stretch.svg", Params::Skip("Not impl: method=stretch")},
                {"mixed-children-1.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-mixed-children-1.png")
                     .withReason("AA diffs")},
                {"mixed-children-2.svg", Params::Skip("Bug: Kerning on textPath")},
                {"nested.svg",
                 Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-nested.png")
                     .withReason("Minor char")},
                {"path-with-ClosePath.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-path-with-ClosePath.png")
                     .withReason("Minor char")},
                {"side=right.svg", Params::Skip("Not impl: side=right (SVG 2)")},
                {"simple-case.svg", Params::WithGoldenOverride(
                                        "donner/svg/renderer/testdata/golden/resvg-simple-case.png")
                                        .withReason("Minor char")},
                {"spacing=auto.svg", Params::Skip("Not impl: spacing=auto")},
                {"startOffset=-100.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-startOffset=-100.png")
                     .withReason("Minor char")},
                {"startOffset=10percent.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-startOffset=10percent.png")
                     .withReason("Minor char")},
                {"startOffset=30.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-startOffset=30.png")
                     .withReason("Minor char")},
                {"startOffset=5mm.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-startOffset=5mm.png")
                     .withReason("Minor char")},
                {"tspan-with-absolute-position.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-tspan-with-absolute-position.png")
                     .withReason("Minor char")},
                {"tspan-with-relative-position.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-tspan-with-relative-position.png")
                     .withReason("Minor char")},
                {"two-paths.svg", Params::WithGoldenOverride(
                                      "donner/svg/renderer/testdata/golden/resvg-two-paths.png")
                                      .withReason("Minor char")},
                {"very-long-text.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-very-long-text.png")
                     .withReason("AA diffs")},
                {"with-baseline-shift-and-rotate.svg",
                 Params::RenderOnly("UB: baseline-shift + rotate")},
                {"with-baseline-shift.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-baseline-shift.png")
                     .withReason("Minor char")},
                {"with-coordinates-on-text.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-coordinates-on-text.png")
                     .withReason("Minor char")},
                {"with-coordinates-on-textPath.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-coordinates-on-textPath.png")
                     .withReason("Minor char")},
                {"with-filter.svg", Params::Skip("Not impl: filter on textPath")},
                {"with-invalid-path-and-xlink-href.svg",
                 Params::Skip("Not impl: invalid path + href")},
                {"with-path-and-xlink-href.svg", Params::Skip("Not impl: path + xlink:href")},
                {"with-path.svg", Params::Skip("Not impl: path attr (SVG 2)")},
                {"with-rotate.svg", Params::WithGoldenOverride(
                                        "donner/svg/renderer/testdata/golden/resvg-with-rotate.png")
                                        .withReason("Minor char")},
                {"with-text-anchor.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-text-anchor.png")},
                {"with-transform-on-a-referenced-path.svg",
                 Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                            "resvg-with-transform-on-a-referenced-path.png")
                     .withReason("Minor char")},
                {"with-transform-outside-a-referenced-path.svg",
                 Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                            "resvg-with-transform-outside-a-referenced-path.png")
                     .withReason("Minor char")},
                {"with-underline.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-underline.png")
                     .withReason("Minor char")},
                {"writing-mode=tb.svg", Params::Skip("Deferred: writing-mode=tb on textPath")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTref, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/tref",
            {
                {"link-to-a-complex-text.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"link-to-a-non-text-element.svg",
                 Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"link-to-an-external-file-element.svg",
                 Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"link-to-text.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"position-attributes.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"style-attributes.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"with-a-title-child.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"with-text.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"xml-space.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTspan, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/tspan",
                {
                    {"bidi-reordering.svg", Params::Skip("Not impl: BIDI reordering")},
                    {"mixed-font-size.svg",
                     Params::Skip("Bug: Handling kerning with font size changes")},
                    {"mixed-xml-space-3.svg", Params::Skip("Whitespace-only text nodes lost in")},
                    {"nested-rotate.svg",
                     Params::Skip("Bug: Applying rotation indices across nested tspans")},
                    {"nested-whitespaces.svg", Params().withMaxPixelsDifferent(400).withReason(
                                                   "Vertical axis has different AA")},
                    {"tspan-bbox-2.svg", Params().withMaxPixelsDifferent(900).withReason(
                                             "Crosshair thin-line AA + underline uses")},
                    {"with-clip-path.svg", Params::Skip("Not impl: Interaction with `clip-path`")},
                    {"with-filter.svg", Params::Skip("Not impl: Interaction with `filter`")},
                    {"with-mask.svg", Params::Skip("Not impl: Interaction with `mask`")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextUnicodeBidi, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/unicode-bidi",
                {
                    {"bidi-override.svg",
                     Params::Skip("Not impl: unicode-bidi property (BiDi override)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextWordSpacing, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("text/word-spacing",
                                        {
                                            {"large-negative.svg",
                                             Params::RenderOnly("UB: word-spacing=-10000")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextWritingMode, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/writing-mode",
                {
                    {"arabic-with-rl.svg", Params::Skip("Non-ascii text").onlyTextFull()},
                    {"inheritance.svg", Params().withMaxPixelsDifferent(1500).withReason(
                                            "Bug: Baseline is ~2px off compared to resvg")},
                    {"japanese-with-tb.svg",
                     Params().onlyTextFull().withMaxPixelsDifferent(600).withReason(
                         "Non-ascii text, bug: y position is ~1px off")},
                    {"mixed-languages-with-tb-and-underline.svg",
                     Params::Skip("Non-ascii text, bug: underline not").onlyTextFull()},
                    {"mixed-languages-with-tb.svg",
                     Params::Skip("Non-ascii text, bug: mixed language").onlyTextFull()},
                    {"tb-and-punctuation.svg",
                     Params::Skip("Non-ascii text, bug: CJK punctuation").onlyTextFull()},
                    {"tb-rl.svg", Params().withMaxPixelsDifferent(1500).withReason(
                                      "Bug: Baseline is ~2px off compared to resvg")},
                    {"tb-with-alignment.svg", Params().withMaxPixelsDifferent(1500).withReason(
                                                  "Bug: Baseline is ~2px off compared to resvg")},
                    {"tb-with-dx-on-second-tspan.svg",
                     Params::Skip("Bug: `writing-mode=tb` with `dx`")},
                    {"tb-with-dx-on-tspan.svg", Params::Skip("Bug: `writing-mode=tb` with `dx`")},
                    {"tb-with-dy-on-second-tspan.svg",
                     Params::Skip("Bug: `writing-mode=tb` with `dy`")},
                    {"tb-with-rotate-and-underline.svg",
                     Params::RenderOnly("UB: tb with rotate and underline")},
                    {"tb-with-rotate.svg", Params::RenderOnly("UB: tb with rotate")},
                    {"tb.svg", Params().withMaxPixelsDifferent(1500).withReason(
                                   "Bug: Baseline is ~2px off compared to resvg")},

                    {"vertical-lr.svg",
                     Params::Skip("Bug: writing-mode edge cases beyond basic support")},
                    {"vertical-rl.svg",
                     Params::Skip("Bug: writing-mode edge cases beyond basic support")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

}  // namespace donner::svg
