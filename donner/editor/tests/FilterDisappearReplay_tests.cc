/// @file
///
/// Regression test for "drag a filtered element, then drag a different
/// element, and the first element's filter output disappears from the
/// live drag view". Replays `filter_elm_disappear.rnr` and reproduces
/// the user-reported bug directly: at mid-drag-2 the test software-
/// composites `bg + promoted_at_translation + fg` (the same three
/// bitmaps the GPU composites for the user's live view) and asserts
/// that the first-dragged filter element's post-drag-1 canvas region
/// still has visible content in that composite. The bug signature is
/// a diff-pixel count that collapses to near-zero inside those
/// bounds — the filter layer vanished from the mid-drag composited
/// output.
///
/// This is a *live-bug* repro, not a settle-render check. The
/// settle-render path (selection cleared → compositor demotes → full
/// flat re-rasterize) produces correct pixels regardless of the
/// split-layer cache state, so testing it would silently mask the
/// bug. Compositing the split layers by hand is the closest we can
/// get to what the user sees without dragging in a real GL context.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/editor/backend_lib/AsyncRenderer.h"
#include "donner/editor/tests/ReproReplayHarness.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

// Recording layout (from `filter_elm_disappear_minimal.rnr`, a
// 9-frame trim of the original user `.rnr`):
//   f=0    viewport init + cold render baseline
//   f=514  drag 1 mdown (filter element)
//   f=520  drag 1 move
//   f=551  drag 1 mup
//   f=692  drag 2 mdown (new drag target)
//   f=700  drag 2 move
//   f=709  mid-drag-2 checkpoint
//   f=715  drag 2 move (past checkpoint)
//   f=726  drag 2 mup
//
// The drag delta at any kept frame is computed from
// `currentMousePos - mdownMousePos`, so skipping intermediate move
// frames doesn't alter the DOM state at the checkpoint — the mid-drag
// transform, the promoted layer's rasterize state, and the split-layer
// cache all match what the full recording would produce. The original
// `filter_elm_disappear.rnr` is preserved for humans inspecting the
// recording or rerunning with a longer timeline.
constexpr std::uint64_t kMidDrag2Frame = 709;

// Alpha-over composite `src` onto `dst` in premultiplied RGBA8 space.
// Formula: dst = src + dst * (1 - src.a).
void CompositeOver(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src,
                   std::size_t pixelCount) {
  ASSERT_EQ(dst.size(), src.size());
  ASSERT_EQ(dst.size(), pixelCount * 4u);
  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t o = i * 4u;
    const unsigned sa = src[o + 3];
    const unsigned inv = 255u - sa;
    for (int c = 0; c < 4; ++c) {
      const unsigned s = src[o + c];
      const unsigned d = dst[o + c];
      // Round-to-nearest `d * inv / 255` without a divide.
      const unsigned product = d * inv;
      const unsigned scaled = (product + 128u + (product >> 8)) >> 8;
      const unsigned out = std::min(255u, s + scaled);
      dst[o + c] = static_cast<uint8_t>(out);
    }
  }
}

// Composite `bg` + `promoted` shifted by `(offsetX, offsetY)` canvas
// pixels + `fg`, all premultiplied RGBA8 at the same canvas
// dimensions. Returns the flat composite.
svg::RendererBitmap CompositeSplitLayers(const svg::RendererBitmap& bg,
                                         const svg::RendererBitmap& promoted,
                                         const svg::RendererBitmap& fg, int offsetX, int offsetY) {
  EXPECT_EQ(bg.dimensions, fg.dimensions);
  EXPECT_EQ(bg.dimensions, promoted.dimensions);
  EXPECT_EQ(bg.rowBytes, fg.rowBytes);
  EXPECT_EQ(bg.rowBytes, promoted.rowBytes);

  const int w = bg.dimensions.x;
  const int h = bg.dimensions.y;
  const std::size_t strideBytes = bg.rowBytes;
  const std::size_t pixelCount = static_cast<std::size_t>(w) * h;

  svg::RendererBitmap out;
  out.dimensions = bg.dimensions;
  out.rowBytes = bg.rowBytes;
  out.alphaType = svg::AlphaType::Premultiplied;
  out.pixels = bg.pixels;  // start with bg

  // Translate `promoted` by `(offsetX, offsetY)`. Build a shifted
  // copy sized to the canvas so `CompositeOver` can run as-is. Out-
  // of-bounds source rows/columns become transparent.
  std::vector<uint8_t> promotedShifted(bg.pixels.size(), 0u);
  for (int y = 0; y < h; ++y) {
    const int srcY = y - offsetY;
    if (srcY < 0 || srcY >= h) continue;
    for (int x = 0; x < w; ++x) {
      const int srcX = x - offsetX;
      if (srcX < 0 || srcX >= w) continue;
      const std::size_t dstOff =
          static_cast<std::size_t>(y) * strideBytes + static_cast<std::size_t>(x) * 4u;
      const std::size_t srcOff =
          static_cast<std::size_t>(srcY) * strideBytes + static_cast<std::size_t>(srcX) * 4u;
      promotedShifted[dstOff + 0] = promoted.pixels[srcOff + 0];
      promotedShifted[dstOff + 1] = promoted.pixels[srcOff + 1];
      promotedShifted[dstOff + 2] = promoted.pixels[srcOff + 2];
      promotedShifted[dstOff + 3] = promoted.pixels[srcOff + 3];
    }
  }
  CompositeOver(out.pixels, promotedShifted, pixelCount);
  CompositeOver(out.pixels, fg.pixels, pixelCount);
  return out;
}

// Count pixels inside `rect` where any RGBA channel of `a` vs `b`
// differs by more than `threshold` (L∞).
int CountDifferingPixelsInRect(const svg::RendererBitmap& a, const svg::RendererBitmap& b,
                               int rectMinX, int rectMinY, int rectMaxX, int rectMaxY,
                               int threshold = 8) {
  if (a.dimensions != b.dimensions || a.rowBytes != b.rowBytes ||
      a.pixels.size() != b.pixels.size()) {
    return -1;
  }
  const int w = a.dimensions.x;
  const int h = a.dimensions.y;
  const int x0 = std::max(0, rectMinX);
  const int y0 = std::max(0, rectMinY);
  const int x1 = std::min(w, rectMaxX);
  const int y1 = std::min(h, rectMaxY);
  const std::size_t strideBytes = a.rowBytes;
  int differing = 0;
  for (int y = y0; y < y1; ++y) {
    const std::size_t rowOff = static_cast<std::size_t>(y) * strideBytes;
    for (int x = x0; x < x1; ++x) {
      const std::size_t px = rowOff + static_cast<std::size_t>(x) * 4u;
      const int dr = std::abs(static_cast<int>(a.pixels[px + 0]) - b.pixels[px + 0]);
      const int dg = std::abs(static_cast<int>(a.pixels[px + 1]) - b.pixels[px + 1]);
      const int db = std::abs(static_cast<int>(a.pixels[px + 2]) - b.pixels[px + 2]);
      const int da = std::abs(static_cast<int>(a.pixels[px + 3]) - b.pixels[px + 3]);
      if (std::max({dr, dg, db, da}) > threshold) {
        ++differing;
      }
    }
  }
  return differing;
}

// Replay the recording in one of two modes and return the captured
// mid-drag-2 flat-composited bitmap plus metadata. `useCompositedPath`
// toggles between the composited split-layer path (what the live
// editor runs) and a full re-rasterize baseline.
struct MidDrag2Capture {
  svg::RendererBitmap flatComposite;  // ground truth for compare
  svg::RendererBitmap bg;
  svg::RendererBitmap promoted;
  svg::RendererBitmap fg;
  Vector2d promotedTranslationDoc = Vector2d::Zero();
  Box2d filterBoundsDoc;
  Transform2d canvasFromDoc;
};

MidDrag2Capture CaptureMidDrag2(const std::filesystem::path& reproPath,
                                const std::filesystem::path& svgPath, bool useCompositedPath) {
  MidDrag2Capture out;

  svg::RendererBitmap bg;
  svg::RendererBitmap promoted;
  svg::RendererBitmap fg;
  svg::RendererBitmap flat;
  Vector2d promotedTranslationDoc = Vector2d::Zero();
  bool midLanded = false;

  ReplayConfig config;
  config.forceCompositedDragPreview = useCompositedPath;
  config.checkpointFrames = {kMidDrag2Frame};
  config.onCheckpoint = [&](std::size_t idx, const RenderResult* result) {
    if (idx != 0 || result == nullptr) return;
    flat = result->bitmap;  // main-renderer flat output
    if (result->compositedPreview.has_value()) {
      bg = result->compositedPreview->backgroundBitmap;
      promoted = result->compositedPreview->promotedBitmap;
      fg = result->compositedPreview->foregroundBitmap;
      promotedTranslationDoc = result->compositedPreview->promotedTranslationDoc;
    }
    midLanded = true;
  };

  ReplayResults r = ReplayRepro(reproPath, svgPath, config);
  EXPECT_GE(r.mouseDownFrameIndices.size(), 2u);
  EXPECT_TRUE(midLanded);
  EXPECT_GE(r.selectionWorldBoundsAtMouseUp.size(), 1u);

  out.canvasFromDoc = r.canvasFromDocumentAtEnd;
  if (!r.selectionWorldBoundsAtMouseUp.empty() &&
      r.selectionWorldBoundsAtMouseUp.front().has_value()) {
    out.filterBoundsDoc = *r.selectionWorldBoundsAtMouseUp.front();
  }

  if (useCompositedPath) {
    // Composited path: flat bitmap stays stale during split-layer
    // drag; reconstruct the GPU composite in software using the
    // same split layers + canvas-pixel offset the ImGui draw list
    // applies on the editor display path.
    const double canvasPerDocX = r.canvasFromDocumentAtEnd.data[0];
    const int offsetX = static_cast<int>(std::round(promotedTranslationDoc.x * canvasPerDocX));
    const int offsetY = static_cast<int>(std::round(promotedTranslationDoc.y * canvasPerDocX));
    out.flatComposite = CompositeSplitLayers(bg, promoted, fg, offsetX, offsetY);
    out.bg = bg;
    out.promoted = promoted;
    out.fg = fg;
    out.promotedTranslationDoc = promotedTranslationDoc;
  } else {
    // Full re-rasterize path: flat bitmap IS the live scene at
    // mid-drag-2 — use it directly as ground truth.
    out.flatComposite = flat;
    out.bg = bg;
    out.promoted = promoted;
    out.fg = fg;
    out.promotedTranslationDoc = promotedTranslationDoc;
  }

  return out;
}

TEST(FilterDisappearReplayTest, CompositedMidDrag2MatchesFullRerasterize) {
  const std::filesystem::path reproPath =
      "donner/editor/tests/filter_elm_disappear_minimal.rnr";
  const std::filesystem::path svgPath = "donner_splash.svg";

  if (!std::filesystem::exists(reproPath) || !std::filesystem::exists(svgPath)) {
    GTEST_SKIP() << "Required data files not available in runfiles: " << reproPath << " or "
                 << svgPath;
  }

  // Ground truth: a full re-rasterize at mid-drag-2. No compositor
  // split-layer cache in play — every frame draws the SVG from
  // scratch with current DOM state, so mid-drag-2 pixels reflect
  // exactly what the scene should look like at that moment.
  const MidDrag2Capture truth = CaptureMidDrag2(reproPath, svgPath, /*useCompositedPath=*/false);

  // Subject under test: the composited drag path — bg + promoted
  // (shifted) + fg, software-composited with the same over-operator
  // the GPU uses.
  const MidDrag2Capture composited =
      CaptureMidDrag2(reproPath, svgPath, /*useCompositedPath=*/true);

  ASSERT_FALSE(truth.flatComposite.empty());
  ASSERT_FALSE(composited.flatComposite.empty());
  ASSERT_EQ(truth.flatComposite.dimensions, composited.flatComposite.dimensions);

  // Sample the first-dragged filter element's post-drag-1 bounds.
  // Drag 2 moves a different element, so the filter stays at its
  // post-drag-1 position through mid-drag-2. Pad for filter output
  // bleed.
  const Box2d filterBoundsCanvas = truth.canvasFromDoc.transformBox(truth.filterBoundsDoc);
  const int padPx = 16;
  const int rectMinX = static_cast<int>(std::floor(filterBoundsCanvas.topLeft.x)) - padPx;
  const int rectMinY = static_cast<int>(std::floor(filterBoundsCanvas.topLeft.y)) - padPx;
  const int rectMaxX = static_cast<int>(std::ceil(filterBoundsCanvas.bottomRight.x)) + padPx;
  const int rectMaxY = static_cast<int>(std::ceil(filterBoundsCanvas.bottomRight.y)) + padPx;
  const int rectArea = std::max(0, (rectMaxX - rectMinX) * (rectMaxY - rectMinY));
  ASSERT_GT(rectArea, 0);

  // Dump everything for inspection.
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR"); dir != nullptr) {
    const std::string base(dir);
    const auto dump = [&](const char* name, const svg::RendererBitmap& bm) {
      if (bm.empty()) return;
      const std::string path = base + "/" + name;
      svg::RendererImageIO::writeRgbaPixelsToPngFile(path.c_str(), bm.pixels, bm.dimensions.x,
                                                     bm.dimensions.y, bm.rowBytes / 4u);
    };
    dump("mid_drag2_truth.png", truth.flatComposite);
    dump("mid_drag2_composited.png", composited.flatComposite);

    // Also dump the pixel-diff so a failing test has something to
    // look at immediately.
    svg::RendererBitmap diff = truth.flatComposite;
    for (std::size_t i = 0; i < diff.pixels.size(); i += 4) {
      const int dr = std::abs(int(truth.flatComposite.pixels[i + 0]) -
                              int(composited.flatComposite.pixels[i + 0]));
      const int dg = std::abs(int(truth.flatComposite.pixels[i + 1]) -
                              int(composited.flatComposite.pixels[i + 1]));
      const int db = std::abs(int(truth.flatComposite.pixels[i + 2]) -
                              int(composited.flatComposite.pixels[i + 2]));
      const uint8_t mag = static_cast<uint8_t>(std::min(255, std::max({dr, dg, db}) * 4));
      diff.pixels[i + 0] = mag;
      diff.pixels[i + 1] = mag;
      diff.pixels[i + 2] = mag;
      diff.pixels[i + 3] = 255;
    }
    dump("mid_drag2_diff.png", diff);
  }

  // Count pixels in the filter's post-drag-1 canvas bounds where the
  // composited path differs significantly from ground truth. Anything
  // above a small tolerance (16 bytes L∞, ~6%) is a real divergence.
  const int differingInFilter = CountDifferingPixelsInRect(
      truth.flatComposite, composited.flatComposite, rectMinX, rectMinY, rectMaxX, rectMaxY, 16);
  ASSERT_GE(differingInFilter, 0);

  // Tolerance: up to 5% of the filter's bounding region can drift —
  // AA edges, filter-kernel rounding under different compose paths,
  // etc. A real "filter disappeared" regression blows past this by
  // orders of magnitude (the whole filter region reads as background
  // in the composited path).
  const int tolerance = std::max(128, rectArea / 20);
  EXPECT_LE(differingInFilter, tolerance)
      << "composited mid-drag-2 output diverges from full-re-rasterize ground truth "
         "inside the first-dragged filter element's post-drag-1 canvas bounds ("
      << rectMinX << "," << rectMinY << " -> " << rectMaxX << "," << rectMaxY
      << ", area=" << rectArea << " px). Mismatched pixels: " << differingInFilter
      << " (tolerance: " << tolerance
      << "). The composited drag path is dropping or corrupting filter content "
         "the full-rasterize path renders correctly — this is the live "
         "\"filter element disappears after dragging another element\" bug. "
         "Inspect `mid_drag2_composited.png`, `mid_drag2_truth.png`, and "
         "`mid_drag2_diff.png` in the test's outputs dir.";
}

}  // namespace
}  // namespace donner::editor
