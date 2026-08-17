#pragma once

/// @file SpanCapture.h
/// @brief Records the coverage a scan conversion produces so it can be replayed later.
///
/// Scan conversion is the expensive half of a fill or stroke: flattening curves, building and
/// sorting edges, and accumulating per-scanline coverage. Its entire pixel effect is the
/// sequence of Blitter calls it makes, so recording that sequence once and replaying it
/// reproduces the draw without re-running any of that work. Coverage and color stay separate:
/// the recorded runs carry coverage only, and the paint is supplied again at replay, so
/// re-coloring a shape costs nothing.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "tiny_skia/Blitter.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Mask.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia/Transform.h"

namespace tiny_skia {

/// Selects which Blitter call a CapturedSpan replays.
enum class SpanOp : std::uint8_t {
  BlitH,           ///< blitH(x, y, length).
  BlitAntiHFirst,  ///< First run of a blitAntiH call.
  BlitAntiHNext,   ///< Continuation run of the preceding BlitAntiHFirst.
  BlitV,           ///< blitV(x, y, length, alpha0).
  BlitAntiH2,      ///< blitAntiH2(x, y, alpha0, alpha1).
  BlitAntiV2,      ///< blitAntiV2(x, y, alpha0, alpha1).
  BlitRect,        ///< blitRect({x, y, length, height}).
  BlitAntiRect,    ///< blitAntiRect(x, y, length, height, alpha0, alpha1).
};

/// One recorded blit call, packed into 16 bytes.
///
/// The record keeps the call identity rather than flattening everything into single-coverage
/// `{x, y, length, coverage}` spans. Flattening does not survive byte-identity: `blitAntiH2`
/// and `blitAntiV2` carry two coverages that one call must apply together, `blitAntiRect`
/// carries a signed origin plus a left and a right edge coverage, and `blitRect` and `blitV`
/// span a height. Splitting those into per-pixel spans would enter the blitter a different
/// number of times with different arguments, and a blitter's per-call fast paths (row fills,
/// two-pixel coverage runs, whole-column writes) do not decompose to the same bytes. Replaying
/// the same calls in the same order keeps identity true by construction instead of by
/// argument.
///
/// Coordinates are stored as signed 32-bit values because the scan converter calls
/// `blitAntiRect` with a left edge one pixel outside the fill, which can be -1, and because
/// the unsigned coordinate methods round-trip exactly through the same 32 bits. Lengths are
/// 16-bit: run lengths are already `std::uint16_t` in the scan converter's own coverage
/// representation, and every other length is bounded by the surface a draw tiles down to. A
/// value that does not fit marks the capture invalid rather than truncating.
struct CapturedSpan {
  std::int32_t x = 0;        ///< Device x, as the same 32 bits the blit call carried.
  std::int32_t y = 0;        ///< Device y, as the same 32 bits the blit call carried.
  std::uint16_t length = 0;  ///< Run/rect width, or height for BlitV.
  std::uint16_t height = 0;  ///< Height for BlitRect and BlitAntiRect, otherwise zero.
  std::uint8_t alpha0 = 0;   ///< Coverage, first coverage of a pair, or left edge coverage.
  std::uint8_t alpha1 = 0;   ///< Second coverage of a pair, or right edge coverage.
  SpanOp op = SpanOp::BlitH;
  std::uint8_t reserved = 0;  ///< Explicit padding so the record compares byte for byte.

  friend bool operator==(const CapturedSpan&, const CapturedSpan&) = default;
};

static_assert(sizeof(CapturedSpan) == 16, "CapturedSpan is a packed 16-byte record");

/// The recorded blit sequence of one draw, in emission order.
///
/// Replay depends on order, so the runs are stored exactly as they were emitted rather than
/// sorted. Clearing keeps the allocation, so a shape that is invalidated and rebuilt settles
/// into allocation-free captures.
class CapturedSpans {
 public:
  /// Drops the recorded runs but keeps the allocation and clears the invalid flag.
  void clear() {
    spans_.clear();
    overflowed_ = false;
  }

  /// Returns true when nothing was recorded, which is what an empty draw produces.
  [[nodiscard]] bool empty() const { return spans_.empty(); }

  /// Returns the number of recorded runs.
  [[nodiscard]] std::size_t size() const { return spans_.size(); }

  /// Returns the recorded runs in emission order.
  [[nodiscard]] std::span<const CapturedSpan> spans() const { return spans_; }

  /// Returns the bytes the recorded runs occupy, excluding unused vector capacity.
  [[nodiscard]] std::size_t byteSize() const { return spans_.size() * sizeof(CapturedSpan); }

  /// Returns false when a blit call carried a value the packed record cannot hold. The
  /// recorded runs are then incomplete and must not be replayed.
  [[nodiscard]] bool valid() const { return !overflowed_; }

 private:
  friend class SpanCaptureBlitter;

  std::vector<CapturedSpan> spans_;
  bool overflowed_ = false;
};

/// Records every blit call it receives and forwards it unchanged to another blitter.
///
/// Forwarding happens in the same call, so a draw renders and records in one pass and the
/// recording costs only the run stores. The wrapped blitter receives the original arguments
/// untouched, so a draw through this blitter paints exactly what it would have painted
/// without it.
class SpanCaptureBlitter final : public Blitter {
 public:
  /// @param wrapped Blitter each call is forwarded to. Must outlive this object.
  /// @param out Destination for the recorded runs. Appended to, not cleared. Must outlive
  ///   this object.
  SpanCaptureBlitter(Blitter& wrapped, CapturedSpans& out) : wrapped_(wrapped), out_(out) {}

  void blitH(std::uint32_t x, std::uint32_t y, LengthU32 width) override;
  void blitAntiH(std::uint32_t x, std::uint32_t y, std::span<std::uint8_t> alpha,
                 std::span<AlphaRun> runs) override;
  void blitV(std::uint32_t x, std::uint32_t y, LengthU32 height, AlphaU8 alpha) override;
  void blitAntiH2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) override;
  void blitAntiV2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) override;
  void blitAntiRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                    AlphaU8 leftAlpha, AlphaU8 rightAlpha) override;
  void blitRect(const ScreenIntRect& rect) override;
  void blitMask(const Mask& mask, const ScreenIntRect& clip) override;

 private:
  /// Appends one run, marking the capture invalid if a value does not fit the packed record.
  void record(const CapturedSpan& span);

  Blitter& wrapped_;
  CapturedSpans& out_;
};

/// Replays recorded runs through `blitter`, in the order they were captured.
///
/// The runs carry coverage only, so `blitter` supplies the color. Replaying through a blitter
/// built from the paint the capture used reproduces the captured draw byte for byte.
void replaySpans(const CapturedSpans& spans, Blitter& blitter);

/// Draws that record their coverage while painting, and the matching replay.
///
/// Each capture entry point performs the same draw `Painter` would and additionally records
/// its blit sequence, so the captured frame is a real frame rather than a dry run. One
/// instance holds the runs of one draw; capturing again into the same instance reuses the
/// allocation, so a shape that is invalidated and re-captured settles into allocation-free
/// rebuilds.
class SpanCapture {
 public:
  /// Fills a rectangle, recording its coverage. See Painter::fillRect.
  ///
  /// @return false if the draw could not be recorded, in which case the pixels are still
  ///   painted and the recorded runs are discarded.
  bool fillRect(MutablePixmapView& pixmap, const Rect& rect, const Paint& paint,
                Transform transform = Transform::identity(), const Mask* mask = nullptr);

  /// Fills a path, recording its coverage. See Painter::fillPath and fillRect's return value.
  bool fillPath(MutablePixmapView& pixmap, const Path& path, const Paint& paint, FillRule fillRule,
                Transform transform = Transform::identity(), const Mask* mask = nullptr);

  /// Strokes a path, recording the stroked outline's coverage. Dashing, caps, and joins are
  /// baked into the recorded runs. See Painter::strokePath and fillRect's return value.
  bool strokePath(MutablePixmapView& pixmap, const Path& path, const Paint& paint,
                  const Stroke& stroke, Transform transform = Transform::identity(),
                  const Mask* mask = nullptr);

  /// The runs the last successful capture recorded, in device space.
  [[nodiscard]] const CapturedSpans& spans() const { return spans_; }

  /// The paint the last successful capture's draw built its blitter from.
  ///
  /// A draw may adjust the caller's paint (hairline strokes fold their coverage into the
  /// shader opacity, and a transformed draw transforms the shader), so replaying with this
  /// paint is what reproduces the draw. Replaying with a different paint of the same kind
  /// re-colors the same coverage.
  [[nodiscard]] const Paint& paint() const { return paint_; }

  /// Replays recorded runs onto `pixmap` with `paint`, through the blitter configuration a
  /// direct draw with that paint would build.
  ///
  /// Gradients need no separate treatment here. A gradient lives in the `Paint`'s shader, and
  /// both a direct draw and a replay hand that shader to the same blitter constructor, which
  /// resolves the stops into pipeline state. Keeping the instantiated shader in the `Paint`
  /// is therefore all a replay needs, and it keeps the two paths sharing one resolution step
  /// instead of two that must be proven equal. If a quantized ramp lookup table is ever
  /// introduced, it has to live inside that shared blitter construction for the same reason:
  /// a ramp built only on one of the two paths would diverge silently.
  ///
  /// @return false if `spans` is invalid, in which case nothing is drawn.
  static bool replay(MutablePixmapView& pixmap, const CapturedSpans& spans, const Paint& paint,
                     const Mask* mask = nullptr);

 private:
  /// Runs one capture pass over `draw` and keeps the result if it is replayable.
  template <typename DrawFn>
  bool capture(const MutablePixmapView& pixmap, const Paint& paint, DrawFn&& draw);

  CapturedSpans spans_;
  Paint paint_;
};

}  // namespace tiny_skia
