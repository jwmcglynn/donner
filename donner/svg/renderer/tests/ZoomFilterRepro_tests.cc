/// @file
/// Regression test for the "zoom-in-enough on splash → filters disappear"
/// bug surfaced via the editor.
///
/// Two independent size caps in `third_party/tiny-skia-cpp` silently no-op'd
/// renderer / filter work above 64 MiB:
///
///   - `Pixmap::fromSize` (and `Mask::fromSize`, `FloatPixmap::fromSize`,
///     `FloatPixmap::fromPixmap`) rejected allocations > 64 MiB →
///     viewport-sized main pixmaps above 4096×4096 RGBA failed and
///     `takeSnapshot` returned empty.
///   - `tiny_skia::filter::gaussianBlur(FloatPixmap&, …)` (and the uint8
///     overload, and `Morphology`) returned early if the buffer exceeded
///     64 MiB → the SourceGraphic flowed through unblurred, so the user
///     saw lightning bolts rendered as hard paths instead of soft halos.
///
/// The user's editor repro: open the splash at default zoom, pinch-zoom
/// in past ~9.18× (where the editor's `kMaxCanvasDim=8192` clamp kicks in
/// on the 892×512 splash), and the blue glows around the bolts vanish.
///
/// These tests pin the regression at the renderer layer:
///
///   - Tiny pixmap (sanity)         - must always succeed.
///   - 4096×4096 main pixmap        - at the prior 64 MiB cap boundary.
///   - 8192×4708 main pixmap        - what the editor produces at
///                                    `kMaxCanvasDim` on the splash; the
///                                    snapshot must be non-empty.
///   - Halo luma low vs high zoom   - the gaussian blur must apply at both
///                                    canvas sizes. Without the fix, the
///                                    high-zoom probe falls back to the
///                                    raw deep-blue background and the
///                                    luma delta jumps from ≤25 to ~37+.
///
/// All four tests dump PNGs to `$TEST_UNDECLARED_OUTPUTS_DIR` so a
/// failing run gives the operator the actual pixels to look at.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

std::string LoadSplash() {
  std::ifstream stream("donner_splash.svg");
  if (!stream.is_open()) {
    return {};
  }
  std::ostringstream buf;
  buf << stream.rdbuf();
  return buf.str();
}

constexpr std::string_view kTinySvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
         <rect width="100" height="100" fill="red"/>
       </svg>)";

// Helper: parse `source`, set canvas, render, optionally save PNG, return
// the snapshot.
RendererBitmap RenderAt(std::string_view source, int width, int height, const char* outName) {
  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(source, warningSink);
  EXPECT_FALSE(result.hasError()) << "parse failed: " << result.error();
  SVGDocument document = std::move(result.result());
  document.setCanvasSize(width, height);
  Renderer renderer;
  renderer.draw(document);
  if (outName != nullptr) {
    if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
      renderer.save((std::filesystem::path(dir) / outName).string().c_str());
    }
  }
  return renderer.takeSnapshot();
}

// Sample an RGBA pixel from `bitmap` at the given device coords, returning
// `{0, 0, 0, 0}` on out-of-bounds.
std::array<uint8_t, 4> SamplePixel(const RendererBitmap& bitmap, int x, int y) {
  if (x < 0 || y < 0 || x >= bitmap.dimensions.x || y >= bitmap.dimensions.y) {
    return {0, 0, 0, 0};
  }
  const size_t stride =
      bitmap.rowBytes ? bitmap.rowBytes : static_cast<size_t>(bitmap.dimensions.x) * 4;
  const size_t offset = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
  return {bitmap.pixels[offset + 0], bitmap.pixels[offset + 1], bitmap.pixels[offset + 2],
          bitmap.pixels[offset + 3]};
}

// Mean of `|luma(lo) - luma(hi)|` over a patch centered at `(lowX, lowY)`
// in `lowZoom` and the proportionally-scaled point in `highZoom`. Used to
// detect a filter going missing: with the blur on, the halo's luma matches
// at both zoom levels; with the blur off, the high-zoom patch falls back
// to the raw background and luma spikes.
double MeanLumaDelta(const RendererBitmap& lowZoom, const RendererBitmap& highZoom, int lowX,
                     int lowY, int patchRadius) {
  const double scaleX =
      static_cast<double>(highZoom.dimensions.x) / static_cast<double>(lowZoom.dimensions.x);
  const double scaleY =
      static_cast<double>(highZoom.dimensions.y) / static_cast<double>(lowZoom.dimensions.y);

  double sumDelta = 0.0;
  int count = 0;
  for (int dy = -patchRadius; dy <= patchRadius; ++dy) {
    for (int dx = -patchRadius; dx <= patchRadius; ++dx) {
      const auto lo = SamplePixel(lowZoom, lowX + dx, lowY + dy);
      const auto hi = SamplePixel(highZoom, static_cast<int>(std::round((lowX + dx) * scaleX)),
                                  static_cast<int>(std::round((lowY + dy) * scaleY)));
      // ITU-R BT.601 luma.
      const double lumaLo = 0.299 * lo[0] + 0.587 * lo[1] + 0.114 * lo[2];
      const double lumaHi = 0.299 * hi[0] + 0.587 * hi[1] + 0.114 * hi[2];
      sumDelta += std::abs(lumaLo - lumaHi);
      ++count;
    }
  }
  return count > 0 ? sumDelta / count : 0.0;
}

TEST(ZoomFilterRepro, TinyCanvasRendersNonEmptySnapshot) {
  // Sanity: a 100×100 canvas must render. If this fails, something is
  // wrong with the test harness itself, not the cap.
  const RendererBitmap snapshot = RenderAt(kTinySvg, 100, 100, /*outName=*/nullptr);
  EXPECT_FALSE(snapshot.empty()) << "100×100 canvas produced an empty snapshot";
}

TEST(ZoomFilterRepro, SquareCanvasAtPriorCapBoundaryRenders) {
  // 4096×4096 RGBA = exactly 64 MiB. The prior `Pixmap::fromSize` cap was
  // `len.value() > kMaxAllocationBytes` (strict greater-than), so this size
  // is on the boundary and was historically allowed.
  const RendererBitmap snapshot = RenderAt(kTinySvg, 4096, 4096, /*outName=*/nullptr);
  EXPECT_FALSE(snapshot.empty())
      << "4096×4096 canvas (64 MiB exactly) failed to render - the cap may have moved.";
}

TEST(ZoomFilterRepro, SplashAtEditorClampBoundaryRenders) {
  // The editor clamps `desiredCanvasSize` at `kMaxCanvasDim=8192` per axis,
  // so at ~9.18× zoom on the 892×512 splash the canvas becomes 8192×4708
  // (~154 MiB) - above the prior 64 MiB tiny-skia cap. Before the fix,
  // `Pixmap::fromSize` returned `nullopt`, `frame_` was empty,
  // `takeSnapshot` returned an empty bitmap, and the editor (which doesn't
  // propagate the failure) ended up showing a stale or empty rendered
  // image.
  const std::string source = LoadSplash();
  if (source.empty()) {
    GTEST_SKIP() << "donner_splash.svg not in runfiles";
  }
  const RendererBitmap snapshot =
      RenderAt(std::string_view{source}, 8192, 4708, "splash_at_editor_clamp_8192x4708.png");
  EXPECT_FALSE(snapshot.empty())
      << "Splash @ 8192×4708 produced an empty snapshot - main pixmap allocation failed. "
         "Check `Pixmap::fromSize` cap in third_party/tiny-skia-cpp.";
  // Renderer preserves the 892:512 viewBox aspect inside the requested canvas,
  // so width matches exactly and height ends up at floor/round of
  // `8192 * 512 / 892` ≈ 4703.79. Accept the aspect-preserved height range.
  EXPECT_EQ(snapshot.dimensions.x, 8192);
  EXPECT_GE(snapshot.dimensions.y, 4700);
  EXPECT_LE(snapshot.dimensions.y, 4708);
}

TEST(ZoomFilterRepro, SplashHaloSurvivesHighZoom) {
  // The gaussian blur halos around the splash's lightning bolts must
  // survive a high-canvas render. Without the filter-primitive cap raises,
  // `gaussianBlur(FloatPixmap&, …)` silently returns early on viewport-
  // sized SourceGraphic float buffers above ~2048×2048 floats - the blur
  // disappears while every other element renders normally. This produces a
  // luma swing of ~37+ at the Lightning_glow_dark halo probe; with the
  // blur intact, the delta is ≤25.
  const std::string source = LoadSplash();
  if (source.empty()) {
    GTEST_SKIP() << "donner_splash.svg not in runfiles";
  }
  const RendererBitmap lo = RenderAt(std::string_view{source}, 892, 512, "splash_halo_lo.png");
  ASSERT_FALSE(lo.empty());
  const RendererBitmap hi = RenderAt(std::string_view{source}, 8192, 4708, "splash_halo_hi.png");
  ASSERT_FALSE(hi.empty());

  // Probe inside the Lightning_glow_dark halo on the small bottom bolt
  // (low-zoom doc coords near (478, 440)) - the bright cyan glow that's
  // visible in the low-zoom reference. When the gaussian blur no-ops at
  // high zoom this region falls back to the deep-blue background.
  const double delta = MeanLumaDelta(lo, hi, /*lowX=*/478, /*lowY=*/440, /*patchRadius=*/4);
  EXPECT_LT(delta, 25.0) << "Halo luma at low vs high zoom differs by " << delta
                         << " - a gaussian blur on the splash is no-op'ing at the high canvas. "
                            "The blur primitive's defensive size cap is the likely cause: check "
                            "third_party/tiny-skia-cpp/src/tiny_skia/filter/"
                            "{GaussianBlur,Morphology,FloatPixmap}.{cpp,h}.";
}

}  // namespace
}  // namespace donner::svg
