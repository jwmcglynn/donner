#include "tiny_skia/SpanCapture.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Mask.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia/shaders/LinearGradient.h"
#include "tiny_skia/shaders/Shaders.h"

namespace {

using tiny_skia::AlphaRun;
using tiny_skia::AlphaU8;
using tiny_skia::Blitter;
using tiny_skia::CapturedSpan;
using tiny_skia::CapturedSpans;
using tiny_skia::Color;
using tiny_skia::FillRule;
using tiny_skia::GradientStop;
using tiny_skia::LengthU32;
using tiny_skia::LinearGradient;
using tiny_skia::Mask;
using tiny_skia::MutablePixmapView;
using tiny_skia::Paint;
using tiny_skia::Painter;
using tiny_skia::Path;
using tiny_skia::PathBuilder;
using tiny_skia::Pixmap;
using tiny_skia::Point;
using tiny_skia::Rect;
using tiny_skia::ScreenIntRect;
using tiny_skia::SpanCapture;
using tiny_skia::SpanCaptureBlitter;
using tiny_skia::SpanOp;
using tiny_skia::SpreadMode;
using tiny_skia::Stroke;
using tiny_skia::StrokeDash;
using tiny_skia::Transform;

constexpr std::uint32_t kDim = 64;

Pixmap makePixmap(std::uint32_t dim = kDim) {
  auto pixmap = Pixmap::fromSize(dim, dim);
  EXPECT_TRUE(pixmap.has_value());
  return std::move(*pixmap);
}

/// A background with a non-trivial color so blending, not just overwriting, is exercised.
void fillBackground(Pixmap& pixmap) { pixmap.fill(Color::fromRgba8(40, 90, 130, 255)); }

/// A closed curved path with diagonal edges, so the scan converter emits partial coverage.
Path makeCurvedPath(float d) {
  PathBuilder pb;
  pb.moveTo(0.10f * d, 0.14f * d);
  pb.cubicTo(0.30f * d, 0.02f * d, 0.70f * d, 0.02f * d, 0.90f * d, 0.18f * d);
  pb.lineTo(0.78f * d, 0.48f * d);
  pb.quadTo(0.64f * d, 0.90f * d, 0.36f * d, 0.82f * d);
  pb.lineTo(0.18f * d, 0.56f * d);
  pb.cubicTo(0.06f * d, 0.44f * d, 0.05f * d, 0.26f * d, 0.10f * d, 0.14f * d);
  pb.close();
  auto path = pb.finish();
  EXPECT_TRUE(path.has_value());
  return std::move(*path);
}

/// A self-intersecting star, so Winding and EvenOdd differ.
Path makeStarPath(float d) {
  PathBuilder pb;
  pb.moveTo(0.50f * d, 0.05f * d);
  pb.lineTo(0.80f * d, 0.95f * d);
  pb.lineTo(0.02f * d, 0.36f * d);
  pb.lineTo(0.98f * d, 0.36f * d);
  pb.lineTo(0.20f * d, 0.95f * d);
  pb.close();
  auto path = pb.finish();
  EXPECT_TRUE(path.has_value());
  return std::move(*path);
}

Paint makeSolidPaint(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
  Paint paint;
  paint.setColorRgba8(r, g, b, a);
  return paint;
}

Paint makeGradientPaint(float d) {
  std::vector<GradientStop> stops = {
      GradientStop::create(0.0f, Color::fromRgba8(255, 0, 0, 255)),
      GradientStop::create(0.5f, Color::fromRgba8(0, 255, 0, 128)),
      GradientStop::create(1.0f, Color::fromRgba8(0, 0, 255, 255)),
  };
  auto shader = LinearGradient::create(Point::fromXY(0.0f, 0.0f), Point::fromXY(d, d),
                                       std::move(stops), SpreadMode::Pad, Transform::identity());
  EXPECT_TRUE(shader.has_value());
  EXPECT_TRUE(std::holds_alternative<LinearGradient>(*shader));

  Paint paint;
  paint.shader = std::get<LinearGradient>(std::move(*shader));
  return paint;
}

/// Reports the first differing pixel rather than only that the buffers differ.
::testing::AssertionResult pixmapsEqual(const Pixmap& expected, const Pixmap& actual) {
  if (expected.size() != actual.size()) {
    return ::testing::AssertionFailure() << "pixmap sizes differ";
  }

  const auto expectedBytes = expected.data();
  const auto actualBytes = actual.data();
  for (std::size_t i = 0; i < expectedBytes.size(); i += 4) {
    if (expectedBytes[i] == actualBytes[i] && expectedBytes[i + 1] == actualBytes[i + 1] &&
        expectedBytes[i + 2] == actualBytes[i + 2] && expectedBytes[i + 3] == actualBytes[i + 3]) {
      continue;
    }

    const std::size_t pixelIndex = i / 4;
    return ::testing::AssertionFailure()
           << "pixel (" << (pixelIndex % expected.width()) << ", "
           << (pixelIndex / expected.width()) << ") differs: expected rgba("
           << static_cast<int>(expectedBytes[i]) << ", " << static_cast<int>(expectedBytes[i + 1])
           << ", " << static_cast<int>(expectedBytes[i + 2]) << ", "
           << static_cast<int>(expectedBytes[i + 3]) << "), actual rgba("
           << static_cast<int>(actualBytes[i]) << ", " << static_cast<int>(actualBytes[i + 1])
           << ", " << static_cast<int>(actualBytes[i + 2]) << ", "
           << static_cast<int>(actualBytes[i + 3]) << ")";
  }

  return ::testing::AssertionSuccess();
}

/// Counts pixels that differ from both the background and the fully covered color, which is
/// where the low-alpha rounding of the pipeline's store is observable.
std::size_t countChangedPixels(const Pixmap& before, const Pixmap& after) {
  const auto beforeBytes = before.data();
  const auto afterBytes = after.data();
  std::size_t count = 0;
  for (std::size_t i = 0; i < beforeBytes.size(); i += 4) {
    for (std::size_t c = 0; c < 4; ++c) {
      if (beforeBytes[i + c] != afterBytes[i + c]) {
        ++count;
        break;
      }
    }
  }
  return count;
}

/// Counts pixels whose color lies strictly between the background and the fully covered
/// color on the red channel, which only partial coverage can produce.
std::size_t countPartialCoveragePixels(const Pixmap& pixmap, std::uint8_t background,
                                       std::uint8_t covered) {
  const auto bytes = pixmap.data();
  const auto lo = std::min(background, covered);
  const auto hi = std::max(background, covered);
  std::size_t count = 0;
  for (std::size_t i = 0; i < bytes.size(); i += 4) {
    if (bytes[i] > lo && bytes[i] < hi) {
      ++count;
    }
  }
  return count;
}

/// Records the blit calls it receives as human-readable strings, so two call sequences can be
/// compared element by element.
class CallLogBlitter final : public Blitter {
 public:
  void blitH(std::uint32_t x, std::uint32_t y, LengthU32 width) override {
    log("blitH", {x, y, width});
  }

  void blitAntiH(std::uint32_t x, std::uint32_t y, std::span<std::uint8_t> alpha,
                 std::span<AlphaRun> runs) override {
    std::ostringstream os;
    os << "blitAntiH(" << x << ", " << y << ")";
    std::size_t runOffset = 0;
    std::size_t alphaOffset = 0;
    while (runOffset < runs.size() && runs[runOffset].has_value()) {
      const auto run = static_cast<std::uint32_t>(runs[runOffset].value());
      if (run == 0u || alphaOffset >= alpha.size()) {
        break;
      }
      os << " [len=" << run << ", cov=" << static_cast<int>(alpha[alphaOffset]) << "]";
      runOffset += run;
      alphaOffset += run;
    }
    calls_.push_back(os.str());
  }

  void blitV(std::uint32_t x, std::uint32_t y, LengthU32 height, AlphaU8 alpha) override {
    log("blitV", {x, y, height, alpha});
  }

  void blitAntiH2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) override {
    log("blitAntiH2", {x, y, alpha0, alpha1});
  }

  void blitAntiV2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) override {
    log("blitAntiV2", {x, y, alpha0, alpha1});
  }

  void blitAntiRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                    AlphaU8 leftAlpha, AlphaU8 rightAlpha) override {
    std::ostringstream os;
    os << "blitAntiRect(" << x << ", " << y << ", " << width << ", " << height << ", "
       << static_cast<int>(leftAlpha) << ", " << static_cast<int>(rightAlpha) << ")";
    calls_.push_back(os.str());
  }

  void blitRect(const ScreenIntRect& rect) override {
    log("blitRect", {rect.x(), rect.y(), rect.width(), rect.height()});
  }

  void blitMask(const Mask&, const ScreenIntRect&) override { calls_.emplace_back("blitMask"); }

  [[nodiscard]] const std::vector<std::string>& calls() const { return calls_; }

 private:
  void log(const char* name, std::initializer_list<std::uint32_t> args) {
    std::ostringstream os;
    os << name << "(";
    bool first = true;
    for (const auto arg : args) {
      if (!first) {
        os << ", ";
      }
      first = false;
      os << arg;
    }
    os << ")";
    calls_.push_back(os.str());
  }

  std::vector<std::string> calls_;
};

/// Collects the ops a capture recorded.
std::set<SpanOp> opsIn(const CapturedSpans& spans) {
  std::set<SpanOp> ops;
  for (const auto& span : spans.spans()) {
    ops.insert(span.op);
  }
  return ops;
}

/// Runs a draw three ways and asserts all three agree byte for byte:
/// directly, through a capture pass, and by replaying the captured runs.
///
/// The capture pass forwards every blit to the pipeline blitter, so its output proves capture
/// changed nothing about the cold frame. The replay output proves the recorded runs reproduce
/// the frame.
template <typename DirectFn, typename CaptureFn>
void expectCaptureReplayIdentity(DirectFn&& direct, CaptureFn&& capture) {
  auto directPixmap = makePixmap();
  fillBackground(directPixmap);
  auto directView = directPixmap.mutableView();
  direct(directView);

  auto capturePixmap = makePixmap();
  fillBackground(capturePixmap);
  auto captureView = capturePixmap.mutableView();
  auto result = capture(captureView);

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->spans.valid());
  EXPECT_FALSE(result->spans.empty()) << "capture recorded nothing, so identity is vacuous";
  EXPECT_TRUE(pixmapsEqual(directPixmap, capturePixmap)) << "capture altered the cold frame";

  auto replayPixmap = makePixmap();
  fillBackground(replayPixmap);
  auto replayView = replayPixmap.mutableView();
  EXPECT_TRUE(SpanCapture::replay(replayView, result->spans, result->paint));
  EXPECT_TRUE(pixmapsEqual(directPixmap, replayPixmap)) << "replay did not reproduce the frame";

  auto blank = makePixmap();
  fillBackground(blank);
  EXPECT_GT(countChangedPixels(blank, directPixmap), 0u) << "the draw painted nothing";
}

// ---- Packed record ----

TEST(CapturedSpanTest, DefaultRecordsCompareEqual) {
  EXPECT_EQ(CapturedSpan{}, CapturedSpan{});
}

TEST(CapturedSpansTest, ClearKeepsCapacity) {
  CapturedSpans spans;
  CallLogBlitter sink;
  SpanCaptureBlitter capture(sink, spans);
  for (std::uint32_t i = 0; i < 64; ++i) {
    capture.blitH(i, i, 4);
  }
  ASSERT_EQ(spans.size(), 64u);
  EXPECT_EQ(spans.byteSize(), 64u * sizeof(CapturedSpan));

  const auto* dataBefore = spans.spans().data();
  spans.clear();
  EXPECT_TRUE(spans.empty());
  EXPECT_TRUE(spans.valid());

  capture.blitH(0, 0, 4);
  EXPECT_EQ(spans.spans().data(), dataBefore) << "clear() released the allocation";
}

// ---- Call-level identity ----

TEST(SpanCaptureBlitterTest, ReplayReproducesTheCapturedCallSequence) {
  CallLogBlitter forwarded;
  CapturedSpans spans;
  SpanCaptureBlitter capture(forwarded, spans);

  // One call of every method a scan converter can make, including a multi-run blitAntiH.
  capture.blitH(3, 4, 7);
  std::vector<std::uint8_t> alpha = {200, 0, 0, 90, 0, 255, 0, 0, 0};
  const auto run = [](std::uint16_t length) { return AlphaRun{length}; };
  std::vector<AlphaRun> runs = {run(3),       std::nullopt, std::nullopt, run(2),
                                std::nullopt, run(3),       std::nullopt, std::nullopt,
                                std::nullopt};
  capture.blitAntiH(10, 4, alpha, runs);
  capture.blitV(2, 5, 6, 128);
  capture.blitAntiH2(8, 6, 33, 77);
  capture.blitAntiV2(9, 7, 44, 88);
  capture.blitAntiRect(-1, 8, 5, 3, 12, 34);
  capture.blitRect(ScreenIntRect::fromXYWHSafe(1, 9, 4, 2));

  ASSERT_TRUE(spans.valid());

  CallLogBlitter replayed;
  tiny_skia::replaySpans(spans, replayed);

  EXPECT_THAT(replayed.calls(), ::testing::ContainerEq(forwarded.calls()));
  EXPECT_THAT(forwarded.calls(),
              ::testing::Contains("blitAntiH(10, 4) [len=3, cov=200] [len=2, cov=90] "
                                  "[len=3, cov=255]"));
}

TEST(SpanCaptureBlitterTest, ForwardsUnchangedArguments) {
  CallLogBlitter forwarded;
  CapturedSpans spans;
  SpanCaptureBlitter capture(forwarded, spans);

  capture.blitAntiRect(-1, 2, 0, 3, 200, 0);

  EXPECT_THAT(forwarded.calls(), ::testing::ElementsAre("blitAntiRect(-1, 2, 0, 3, 200, 0)"));
  EXPECT_EQ(spans.size(), 1u);
  EXPECT_EQ(spans.spans()[0].x, -1);
}

TEST(SpanCaptureBlitterTest, OversizedRunInvalidatesTheCapture) {
  CallLogBlitter forwarded;
  CapturedSpans spans;
  SpanCaptureBlitter capture(forwarded, spans);

  capture.blitH(0, 0, 100000);

  EXPECT_FALSE(spans.valid());
  EXPECT_THAT(forwarded.calls(), ::testing::ElementsAre("blitH(0, 0, 100000)"))
      << "an unrecordable run must still be painted";
}

TEST(SpanCaptureBlitterTest, BlitMaskInvalidatesTheCapture) {
  auto mask = Mask::fromSize(4, 4);
  ASSERT_TRUE(mask.has_value());

  CallLogBlitter forwarded;
  CapturedSpans spans;
  SpanCaptureBlitter capture(forwarded, spans);

  capture.blitMask(*mask, ScreenIntRect::fromXYWHSafe(0, 0, 4, 4));

  EXPECT_FALSE(spans.valid());
  EXPECT_THAT(forwarded.calls(), ::testing::ElementsAre("blitMask"));
}

TEST(SpanCaptureTest, ReplayOfAnInvalidCaptureDrawsNothing) {
  CallLogBlitter forwarded;
  CapturedSpans spans;
  SpanCaptureBlitter capture(forwarded, spans);
  capture.blitH(0, 0, 100000);
  ASSERT_FALSE(spans.valid());

  CallLogBlitter replayed;
  tiny_skia::replaySpans(spans, replayed);
  EXPECT_THAT(replayed.calls(), ::testing::IsEmpty());

  auto pixmap = makePixmap();
  fillBackground(pixmap);
  auto before = makePixmap();
  fillBackground(before);
  auto view = pixmap.mutableView();
  EXPECT_FALSE(SpanCapture::replay(view, spans, makeSolidPaint(255, 0, 0, 255)));
  EXPECT_TRUE(pixmapsEqual(before, pixmap));
}

// ---- Byte identity for fills ----

TEST(SpanCaptureTest, FillPathAntiAliased) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  const auto paint = makeSolidPaint(220, 30, 90, 255);

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::fillPath(view, path, paint, FillRule::Winding, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::fillPath(view, path, paint, FillRule::Winding,
                                     Transform::identity());
      });
}

TEST(SpanCaptureTest, FillPathAntiAliasedKeepsLowAlphaEdgePixels) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  const auto paint = makeSolidPaint(220, 30, 90, 255);

  auto directPixmap = makePixmap();
  fillBackground(directPixmap);
  auto directView = directPixmap.mutableView();
  Painter::fillPath(directView, path, paint, FillRule::Winding, Transform::identity());

  // Background red is 40, fully covered red is 220; anything strictly between is an
  // antialiased edge pixel, which is where the store's rounding is observable.
  EXPECT_GT(countPartialCoveragePixels(directPixmap, 40, 220), 100u)
      << "the case does not produce enough partial coverage to be meaningful";

  auto capturePixmap = makePixmap();
  fillBackground(capturePixmap);
  auto captureView = capturePixmap.mutableView();
  auto result = SpanCapture::fillPath(captureView, path, paint, FillRule::Winding,
                                      Transform::identity());
  ASSERT_TRUE(result.has_value());

  auto replayPixmap = makePixmap();
  fillBackground(replayPixmap);
  auto replayView = replayPixmap.mutableView();
  ASSERT_TRUE(SpanCapture::replay(replayView, result->spans, result->paint));

  EXPECT_TRUE(pixmapsEqual(directPixmap, replayPixmap));
}

TEST(SpanCaptureTest, FillPathTranslucent) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  const auto paint = makeSolidPaint(220, 30, 90, 96);

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::fillPath(view, path, paint, FillRule::Winding, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::fillPath(view, path, paint, FillRule::Winding,
                                     Transform::identity());
      });
}

TEST(SpanCaptureTest, FillPathAliased) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  auto paint = makeSolidPaint(220, 30, 90, 255);
  paint.antiAlias = false;

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::fillPath(view, path, paint, FillRule::Winding, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::fillPath(view, path, paint, FillRule::Winding,
                                     Transform::identity());
      });
}

TEST(SpanCaptureTest, FillPathEvenOdd) {
  const auto path = makeStarPath(static_cast<float>(kDim));
  const auto paint = makeSolidPaint(30, 200, 120, 255);

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::fillPath(view, path, paint, FillRule::EvenOdd, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::fillPath(view, path, paint, FillRule::EvenOdd,
                                     Transform::identity());
      });
}

TEST(SpanCaptureTest, FillPathWithGradient) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  const auto paint = makeGradientPaint(static_cast<float>(kDim));

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::fillPath(view, path, paint, FillRule::Winding, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::fillPath(view, path, paint, FillRule::Winding,
                                     Transform::identity());
      });
}

TEST(SpanCaptureTest, FillPathTransformedTransformsTheShaderToo) {
  const auto path = makeCurvedPath(static_cast<float>(kDim) * 0.5f);
  const auto paint = makeGradientPaint(static_cast<float>(kDim) * 0.5f);
  const auto transform = Transform::fromRow(1.4f, 0.3f, -0.2f, 1.1f, 6.0f, 4.0f);

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::fillPath(view, path, paint, FillRule::Winding, transform);
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::fillPath(view, path, paint, FillRule::Winding, transform);
      });
}

TEST(SpanCaptureTest, FillPathWithMask) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  const auto paint = makeSolidPaint(220, 30, 90, 255);

  auto mask = Mask::fromSize(kDim, kDim);
  ASSERT_TRUE(mask.has_value());
  const auto maskRect = Rect::fromXYWH(8.0f, 8.0f, 40.0f, 44.0f);
  ASSERT_TRUE(maskRect.has_value());
  mask->fillPath(Path::fromRect(*maskRect), FillRule::Winding, true, Transform::identity());

  auto directPixmap = makePixmap();
  fillBackground(directPixmap);
  auto directView = directPixmap.mutableView();
  Painter::fillPath(directView, path, paint, FillRule::Winding, Transform::identity(),
                    &(*mask));

  auto capturePixmap = makePixmap();
  fillBackground(capturePixmap);
  auto captureView = capturePixmap.mutableView();
  auto result = SpanCapture::fillPath(captureView, path, paint, FillRule::Winding,
                                      Transform::identity(), &(*mask));
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(pixmapsEqual(directPixmap, capturePixmap));

  auto replayPixmap = makePixmap();
  fillBackground(replayPixmap);
  auto replayView = replayPixmap.mutableView();
  ASSERT_TRUE(SpanCapture::replay(replayView, result->spans, result->paint, &(*mask)));
  EXPECT_TRUE(pixmapsEqual(directPixmap, replayPixmap));
}

// ---- Byte identity for rect fast paths ----

TEST(SpanCaptureTest, FillRectAntiAliasedFractionalEdges) {
  const auto rect = Rect::fromXYWH(6.25f, 9.75f, 41.5f, 33.125f);
  ASSERT_TRUE(rect.has_value());
  const auto paint = makeSolidPaint(250, 210, 20, 255);

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::fillRect(view, *rect, paint, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::fillRect(view, *rect, paint, Transform::identity());
      });
}

TEST(SpanCaptureTest, FillRectAliased) {
  const auto rect = Rect::fromXYWH(6.0f, 9.0f, 41.0f, 33.0f);
  ASSERT_TRUE(rect.has_value());
  auto paint = makeSolidPaint(250, 210, 20, 255);
  paint.antiAlias = false;

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::fillRect(view, *rect, paint, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::fillRect(view, *rect, paint, Transform::identity());
      });
}

TEST(SpanCaptureTest, FillRectAliasedRecordsASingleRect) {
  const auto rect = Rect::fromXYWH(6.0f, 9.0f, 41.0f, 33.0f);
  ASSERT_TRUE(rect.has_value());
  auto paint = makeSolidPaint(250, 210, 20, 255);
  paint.antiAlias = false;

  auto pixmap = makePixmap();
  fillBackground(pixmap);
  auto view = pixmap.mutableView();
  auto result = SpanCapture::fillRect(view, *rect, paint, Transform::identity());

  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(opsIn(result->spans), ::testing::ElementsAre(SpanOp::BlitRect));
  EXPECT_EQ(result->spans.size(), 1u);
}

// ---- Byte identity for strokes and dashes ----

TEST(SpanCaptureTest, StrokePathThick) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  const auto paint = makeSolidPaint(10, 10, 200, 255);
  Stroke stroke;
  stroke.width = 5.5f;
  stroke.lineJoin = tiny_skia::LineJoin::Round;
  stroke.lineCap = tiny_skia::LineCap::Round;

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::strokePath(view, path, paint, stroke, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::strokePath(view, path, paint, stroke, Transform::identity());
      });
}

TEST(SpanCaptureTest, StrokePathHairline) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  const auto paint = makeSolidPaint(10, 10, 200, 255);
  Stroke stroke;
  stroke.width = 0.0f;

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::strokePath(view, path, paint, stroke, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::strokePath(view, path, paint, stroke, Transform::identity());
      });
}

TEST(SpanCaptureTest, StrokePathSubPixelHairlineAdjustsThePaint) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  const auto paint = makeSolidPaint(10, 10, 200, 255);
  Stroke stroke;
  // Below one device pixel, so the stroke is drawn as a hairline whose coverage is folded
  // into the shader opacity. Replay has to use that adjusted paint, not the caller's.
  stroke.width = 0.4f;

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::strokePath(view, path, paint, stroke, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::strokePath(view, path, paint, stroke, Transform::identity());
      });
}

TEST(SpanCaptureTest, StrokePathDashedThick) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  const auto paint = makeSolidPaint(10, 10, 200, 255);
  Stroke stroke;
  stroke.width = 3.0f;
  stroke.dash = StrokeDash::create({6.0f, 4.0f, 2.0f, 4.0f}, 1.5f);
  ASSERT_TRUE(stroke.dash.has_value());

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::strokePath(view, path, paint, stroke, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::strokePath(view, path, paint, stroke, Transform::identity());
      });
}

TEST(SpanCaptureTest, StrokePathDashedHairline) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  const auto paint = makeSolidPaint(10, 10, 200, 255);
  Stroke stroke;
  stroke.width = 0.0f;
  stroke.dash = StrokeDash::create({5.0f, 3.0f}, 0.0f);
  ASSERT_TRUE(stroke.dash.has_value());

  expectCaptureReplayIdentity(
      [&](MutablePixmapView& view) {
        Painter::strokePath(view, path, paint, stroke, Transform::identity());
      },
      [&](MutablePixmapView& view) {
        return SpanCapture::strokePath(view, path, paint, stroke, Transform::identity());
      });
}

// ---- Color at blit ----

TEST(SpanCaptureTest, ReplayWithANewColorMatchesADirectDrawInThatColor) {
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  const auto capturePaint = makeSolidPaint(220, 30, 90, 255);
  const auto replayPaint = makeSolidPaint(15, 190, 240, 200);

  auto capturePixmap = makePixmap();
  fillBackground(capturePixmap);
  auto captureView = capturePixmap.mutableView();
  auto result = SpanCapture::fillPath(captureView, path, capturePaint, FillRule::Winding,
                                      Transform::identity());
  ASSERT_TRUE(result.has_value());

  auto directPixmap = makePixmap();
  fillBackground(directPixmap);
  auto directView = directPixmap.mutableView();
  Painter::fillPath(directView, path, replayPaint, FillRule::Winding, Transform::identity());

  auto replayPixmap = makePixmap();
  fillBackground(replayPixmap);
  auto replayView = replayPixmap.mutableView();
  ASSERT_TRUE(SpanCapture::replay(replayView, result->spans, replayPaint));

  EXPECT_TRUE(pixmapsEqual(directPixmap, replayPixmap));
}

// ---- Interface coverage and limits ----

TEST(SpanCaptureTest, CapturesEveryBlitterMethodTheScanConvertersUse) {
  std::set<SpanOp> seen;

  const auto collect = [&seen](std::optional<SpanCapture::Result> result) {
    ASSERT_TRUE(result.has_value());
    const auto ops = opsIn(result->spans);
    seen.insert(ops.begin(), ops.end());
  };

  const auto curved = makeCurvedPath(static_cast<float>(kDim));
  const auto paint = makeSolidPaint(220, 30, 90, 255);

  auto pixmap = makePixmap();
  auto view = pixmap.mutableView();

  // Antialiased path fill: interior runs, edge coverage pairs, and the axis-aligned span
  // optimization that emits blitAntiRect and blitV.
  const auto axisRect = Rect::fromXYWH(4.5f, 4.5f, 40.0f, 40.0f);
  ASSERT_TRUE(axisRect.has_value());
  collect(SpanCapture::fillPath(view, Path::fromRect(*axisRect), paint, FillRule::Winding,
                                Transform::identity()));
  collect(SpanCapture::fillPath(view, curved, paint, FillRule::Winding, Transform::identity()));

  // Aliased rect fill: a single blitRect.
  auto aliasedPaint = paint;
  aliasedPaint.antiAlias = false;
  const auto rect = Rect::fromXYWH(6.0f, 9.0f, 41.0f, 33.0f);
  ASSERT_TRUE(rect.has_value());
  collect(SpanCapture::fillRect(view, *rect, aliasedPaint, Transform::identity()));

  // Antialiased rect fill with fractional edges: the partial top and bottom rows go through
  // the coverage-run form of the interface.
  const auto fractionalRect = Rect::fromXYWH(6.25f, 9.75f, 41.5f, 33.125f);
  ASSERT_TRUE(fractionalRect.has_value());
  collect(SpanCapture::fillRect(view, *fractionalRect, paint, Transform::identity()));

  // Antialiased hairline stroke: horizontal and vertical coverage pairs.
  Stroke hairline;
  hairline.width = 0.0f;
  collect(SpanCapture::strokePath(view, curved, paint, hairline, Transform::identity()));

  EXPECT_THAT(seen, ::testing::IsSupersetOf({SpanOp::BlitH, SpanOp::BlitAntiHFirst,
                                             SpanOp::BlitV, SpanOp::BlitAntiH2,
                                             SpanOp::BlitAntiV2, SpanOp::BlitRect,
                                             SpanOp::BlitAntiRect}));
}

TEST(SpanCaptureTest, RejectsTiledDraws) {
  // Above the tiling threshold a draw splits into tiles that each address pixels relative to
  // their own origin, which one device-space run list cannot describe.
  auto pixmap = Pixmap::fromSize(9000, 4);
  ASSERT_TRUE(pixmap.has_value());
  auto view = pixmap->mutableView();

  const auto rect = Rect::fromXYWH(1.0f, 1.0f, 100.0f, 2.0f);
  ASSERT_TRUE(rect.has_value());

  EXPECT_FALSE(SpanCapture::fillRect(view, *rect, makeSolidPaint(255, 0, 0, 255),
                                     Transform::identity())
                   .has_value());
}

TEST(SpanCaptureTest, FullyTransparentPaintPaintsNothing) {
  // Coverage is recorded even when the paint contributes no color, because coverage and color
  // are independent: the same runs replayed with an opaque paint must paint the shape.
  const auto path = makeCurvedPath(static_cast<float>(kDim));
  auto pixmap = makePixmap();
  fillBackground(pixmap);
  auto before = makePixmap();
  fillBackground(before);
  auto view = pixmap.mutableView();

  auto result = SpanCapture::fillPath(view, path, makeSolidPaint(220, 30, 90, 0),
                                      FillRule::Winding, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(pixmapsEqual(before, pixmap));

  auto replayPixmap = makePixmap();
  fillBackground(replayPixmap);
  auto replayView = replayPixmap.mutableView();
  EXPECT_TRUE(SpanCapture::replay(replayView, result->spans, result->paint));
  EXPECT_TRUE(pixmapsEqual(before, replayPixmap));
}

}  // namespace
