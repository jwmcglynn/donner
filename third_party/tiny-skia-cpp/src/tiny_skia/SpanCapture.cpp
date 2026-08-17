#include "tiny_skia/SpanCapture.h"

#include <limits>

#include "tiny_skia/Painter.h"
#include "tiny_skia/pipeline/Blitter.h"

namespace tiny_skia {

namespace {

/// Returns true when a blit's length or height fits the packed record's 16-bit fields.
constexpr bool fitsPackedLength(std::uint32_t value) {
  return value <= std::numeric_limits<std::uint16_t>::max();
}

/// Reinterprets an unsigned device coordinate as the signed field of a packed record. The
/// blitter methods disagree on signedness (`blitAntiRect` takes signed coordinates, the rest
/// take unsigned ones), so the record keeps the bits and each replay restores the type its
/// call expects.
constexpr std::int32_t toPackedCoord(std::uint32_t value) {
  return static_cast<std::int32_t>(value);
}

/// Inverse of toPackedCoord.
constexpr std::uint32_t fromPackedCoord(std::int32_t value) {
  return static_cast<std::uint32_t>(value);
}

/// Captures a draw's blit sequence and remembers the paint the draw built its blitter from.
///
/// A tiled draw splits the surface and builds one blitter per tile, each addressing pixels
/// relative to its own tile origin. Recorded runs are device-space, so a multi-tile draw
/// cannot be recorded as one sequence; the capture entry points reject those draws before
/// starting, and this counter is the assertion that they did.
class SpanCaptureWrapper final : public BlitterWrapper {
 public:
  SpanCaptureWrapper(CapturedSpans& out, const Paint& fallbackPaint)
      : out_(out), paint_(fallbackPaint) {}

  Blitter& wrap(Blitter& pipelineBlitter, const Paint& paint) override {
    ++passes_;
    paint_ = paint;
    capture_.emplace(pipelineBlitter, out_);
    return *capture_;
  }

  /// Returns the paint the draw used, or the caller's paint when the draw built no blitter.
  [[nodiscard]] const Paint& paint() const { return paint_; }

  /// Returns true when at most one scan pass ran, so the recorded runs share one origin.
  [[nodiscard]] bool singlePass() const { return passes_ <= 1; }

 private:
  std::optional<SpanCaptureBlitter> capture_;
  CapturedSpans& out_;
  Paint paint_;
  int passes_ = 0;
};

}  // namespace

void SpanCaptureBlitter::record(const CapturedSpan& span) { out_.spans_.push_back(span); }

void SpanCaptureBlitter::blitH(std::uint32_t x, std::uint32_t y, LengthU32 width) {
  if (!fitsPackedLength(width)) {
    out_.overflowed_ = true;
  } else {
    record(CapturedSpan{.x = toPackedCoord(x),
                        .y = toPackedCoord(y),
                        .length = static_cast<std::uint16_t>(width),
                        .op = SpanOp::BlitH});
  }
  wrapped_.blitH(x, y, width);
}

void SpanCaptureBlitter::blitAntiH(std::uint32_t x, std::uint32_t y,
                                   std::span<std::uint8_t> alpha, std::span<AlphaRun> runs) {
  // Walk the coverage runs the same way a consuming blitter does: the run length lives at the
  // start index of each run and a missing entry ends the row. Only the start indices carry
  // meaning, so recording one entry per run is lossless.
  std::size_t runOffset = 0;
  std::size_t alphaOffset = 0;
  std::uint32_t runX = x;
  bool first = true;

  while (runOffset < runs.size() && runs[runOffset].has_value()) {
    const auto run = static_cast<std::uint32_t>(runs[runOffset].value());
    if (run == 0u || alphaOffset >= alpha.size()) {
      break;
    }

    if (!fitsPackedLength(run)) {
      out_.overflowed_ = true;
      break;
    }

    record(CapturedSpan{.x = toPackedCoord(runX),
                        .y = toPackedCoord(y),
                        .length = static_cast<std::uint16_t>(run),
                        .alpha0 = alpha[alphaOffset],
                        .op = first ? SpanOp::BlitAntiHFirst : SpanOp::BlitAntiHNext});
    first = false;

    runX += run;
    runOffset += static_cast<std::size_t>(run);
    alphaOffset += static_cast<std::size_t>(run);
  }

  wrapped_.blitAntiH(x, y, alpha, runs);
}

void SpanCaptureBlitter::blitV(std::uint32_t x, std::uint32_t y, LengthU32 height,
                               AlphaU8 alpha) {
  if (!fitsPackedLength(height)) {
    out_.overflowed_ = true;
  } else {
    record(CapturedSpan{.x = toPackedCoord(x),
                        .y = toPackedCoord(y),
                        .length = static_cast<std::uint16_t>(height),
                        .alpha0 = alpha,
                        .op = SpanOp::BlitV});
  }
  wrapped_.blitV(x, y, height, alpha);
}

void SpanCaptureBlitter::blitAntiH2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0,
                                    AlphaU8 alpha1) {
  record(CapturedSpan{.x = toPackedCoord(x),
                      .y = toPackedCoord(y),
                      .alpha0 = alpha0,
                      .alpha1 = alpha1,
                      .op = SpanOp::BlitAntiH2});
  wrapped_.blitAntiH2(x, y, alpha0, alpha1);
}

void SpanCaptureBlitter::blitAntiV2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0,
                                    AlphaU8 alpha1) {
  record(CapturedSpan{.x = toPackedCoord(x),
                      .y = toPackedCoord(y),
                      .alpha0 = alpha0,
                      .alpha1 = alpha1,
                      .op = SpanOp::BlitAntiV2});
  wrapped_.blitAntiV2(x, y, alpha0, alpha1);
}

void SpanCaptureBlitter::blitAntiRect(std::int32_t x, std::int32_t y, std::int32_t width,
                                      std::int32_t height, AlphaU8 leftAlpha,
                                      AlphaU8 rightAlpha) {
  if (width < 0 || height < 0 || !fitsPackedLength(static_cast<std::uint32_t>(width)) ||
      !fitsPackedLength(static_cast<std::uint32_t>(height))) {
    out_.overflowed_ = true;
  } else {
    record(CapturedSpan{.x = x,
                        .y = y,
                        .length = static_cast<std::uint16_t>(width),
                        .height = static_cast<std::uint16_t>(height),
                        .alpha0 = leftAlpha,
                        .alpha1 = rightAlpha,
                        .op = SpanOp::BlitAntiRect});
  }
  wrapped_.blitAntiRect(x, y, width, height, leftAlpha, rightAlpha);
}

void SpanCaptureBlitter::blitRect(const ScreenIntRect& rect) {
  if (!fitsPackedLength(rect.width()) || !fitsPackedLength(rect.height())) {
    out_.overflowed_ = true;
  } else {
    record(CapturedSpan{.x = toPackedCoord(rect.x()),
                        .y = toPackedCoord(rect.y()),
                        .length = static_cast<std::uint16_t>(rect.width()),
                        .height = static_cast<std::uint16_t>(rect.height()),
                        .op = SpanOp::BlitRect});
  }
  wrapped_.blitRect(rect);
}

void SpanCaptureBlitter::blitMask(const Mask& mask, const ScreenIntRect& clip) {
  // No scan converter calls blitMask; masks reach the blitter as a clip the pipeline applies
  // per blit, not as a separate blit. A caller that drives this blitter with a direct blitMask
  // is asking for pixels the recorded runs cannot describe, so the capture is marked invalid
  // rather than silently losing them. The forwarded call still paints.
  out_.overflowed_ = true;
  wrapped_.blitMask(mask, clip);
}

void replaySpans(const CapturedSpans& spans, Blitter& blitter) {
  if (!spans.valid()) {
    return;
  }

  // Reused across rows so replay allocates at most once per call.
  std::vector<std::uint8_t> alpha;
  std::vector<AlphaRun> runs;

  const auto all = spans.spans();
  for (std::size_t i = 0; i < all.size();) {
    const CapturedSpan& span = all[i];
    switch (span.op) {
      case SpanOp::BlitH:
        blitter.blitH(fromPackedCoord(span.x), fromPackedCoord(span.y), span.length);
        ++i;
        break;

      case SpanOp::BlitAntiHFirst: {
        // Rebuild the row's coverage arrays: a run's length and coverage sit at the run's
        // start index, and a missing entry past the last run ends the row.
        std::size_t end = i + 1;
        std::size_t total = span.length;
        while (end < all.size() && all[end].op == SpanOp::BlitAntiHNext) {
          total += all[end].length;
          ++end;
        }

        alpha.assign(total + 1, 0);
        runs.assign(total + 1, std::nullopt);

        std::size_t offset = 0;
        for (std::size_t r = i; r < end; ++r) {
          runs[offset] = all[r].length;
          alpha[offset] = all[r].alpha0;
          offset += all[r].length;
        }

        blitter.blitAntiH(fromPackedCoord(span.x), fromPackedCoord(span.y), alpha, runs);
        i = end;
        break;
      }

      case SpanOp::BlitAntiHNext:
        // Only reachable if a capture lost its BlitAntiHFirst, which record() cannot produce.
        ++i;
        break;

      case SpanOp::BlitV:
        blitter.blitV(fromPackedCoord(span.x), fromPackedCoord(span.y), span.length,
                      span.alpha0);
        ++i;
        break;

      case SpanOp::BlitAntiH2:
        blitter.blitAntiH2(fromPackedCoord(span.x), fromPackedCoord(span.y), span.alpha0,
                           span.alpha1);
        ++i;
        break;

      case SpanOp::BlitAntiV2:
        blitter.blitAntiV2(fromPackedCoord(span.x), fromPackedCoord(span.y), span.alpha0,
                           span.alpha1);
        ++i;
        break;

      case SpanOp::BlitRect:
        blitter.blitRect(ScreenIntRect::fromXYWHSafe(fromPackedCoord(span.x),
                                                     fromPackedCoord(span.y), span.length,
                                                     span.height));
        ++i;
        break;

      case SpanOp::BlitAntiRect:
        blitter.blitAntiRect(span.x, span.y, span.length, span.height, span.alpha0,
                             span.alpha1);
        ++i;
        break;
    }
  }
}

template <typename DrawFn>
bool SpanCapture::capture(const MutablePixmapView& pixmap, const Paint& paint, DrawFn&& draw) {
  spans_.clear();
  paint_ = paint;

  // A tiled draw would record runs from several tile-local origins into one sequence.
  if (detail::DrawTiler::required(pixmap.width(), pixmap.height())) {
    draw(nullptr);
    return false;
  }

  SpanCaptureWrapper wrapper(spans_, paint);
  draw(&wrapper);

  if (!spans_.valid() || !wrapper.singlePass()) {
    spans_.clear();
    return false;
  }

  paint_ = wrapper.paint();
  return true;
}

bool SpanCapture::fillRect(MutablePixmapView& pixmap, const Rect& rect, const Paint& paint,
                           Transform transform, const Mask* mask) {
  return capture(pixmap, paint, [&](BlitterWrapper* wrapper) {
    Painter::fillRect(pixmap, rect, paint, transform, mask, wrapper);
  });
}

bool SpanCapture::fillPath(MutablePixmapView& pixmap, const Path& path, const Paint& paint,
                           FillRule fillRule, Transform transform, const Mask* mask) {
  return capture(pixmap, paint, [&](BlitterWrapper* wrapper) {
    Painter::fillPath(pixmap, path, paint, fillRule, transform, mask, wrapper);
  });
}

bool SpanCapture::strokePath(MutablePixmapView& pixmap, const Path& path, const Paint& paint,
                             const Stroke& stroke, Transform transform, const Mask* mask) {
  return capture(pixmap, paint, [&](BlitterWrapper* wrapper) {
    Painter::strokePath(pixmap, path, paint, stroke, transform, mask, wrapper);
  });
}

bool SpanCapture::replay(MutablePixmapView& pixmap, const CapturedSpans& spans,
                         const Paint& paint, const Mask* mask) {
  if (!spans.valid()) {
    return false;
  }

  auto submaskOpt = mask ? std::optional<SubMaskView>(mask->submask()) : std::nullopt;
  auto subpix = pixmap.subpixmap();
  auto blitter = pipeline::RasterPipelineBlitter::create(paint, submaskOpt, &subpix);
  if (!blitter.has_value()) {
    // The paint contributes nothing, exactly as a direct draw with it would.
    return true;
  }

  replaySpans(spans, *blitter);
  return true;
}

}  // namespace tiny_skia
