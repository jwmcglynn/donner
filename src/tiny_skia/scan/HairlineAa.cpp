#include "tiny_skia/scan/HairlineAa.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "tiny_skia/FixedPoint.h"
#include "tiny_skia/LineClipper.h"
#include "tiny_skia/scan/Hairline.h"

namespace tiny_skia {

namespace {

struct FixedRect {
  FDot16 left = 0;
  FDot16 top = 0;
  FDot16 right = 0;
  FDot16 bottom = 0;

  static std::optional<FixedRect> fromRect(const Rect& rect) {
    return FixedRect{
        fdot16::fromF32(rect.left()),
        fdot16::fromF32(rect.top()),
        fdot16::fromF32(rect.right()),
        fdot16::fromF32(rect.bottom()),
    };
  }
};

constexpr std::int32_t kMaxCoord = 32767;

void doScanline(FDot8 l, std::int32_t top, FDot8 r, AlphaU8 alpha, Blitter& blitter);
void fillDot8(FDot8 l, FDot8 t, FDot8 r, FDot8 b, bool fillInner, Blitter& blitter);
void callHlineBlitter(std::uint32_t x, const std::optional<std::uint32_t>& y, LengthU32 count,
                      AlphaU8 alpha, Blitter& blitter);
AlphaU8 i32ToAlpha(std::int32_t alpha);

AlphaU8 alphaMul(AlphaU8 value, std::int32_t alpha256) {
  const auto result = (static_cast<std::int32_t>(value) * alpha256) >> 8;
  return static_cast<AlphaU8>(result);
}

std::optional<std::uint32_t> toU32(std::int32_t value) {
  if (value < 0) {
    return std::nullopt;
  }
  return std::make_optional(static_cast<std::uint32_t>(value));
}

std::optional<LengthU32> toLengthU32(std::int32_t value) {
  if (value <= 0) {
    return std::nullopt;
  }
  return static_cast<LengthU32>(value);
}

std::optional<IntRect> makeIntRectFromLTRB(std::int32_t left, std::int32_t top, std::int32_t right,
                                           std::int32_t bottom) {
  const auto width64 = static_cast<std::int64_t>(right) - static_cast<std::int64_t>(left);
  const auto height64 = static_cast<std::int64_t>(bottom) - static_cast<std::int64_t>(top);
  if (width64 <= 0 || height64 <= 0) {
    return std::nullopt;
  }

  if (width64 > std::numeric_limits<std::uint32_t>::max() ||
      height64 > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }

  return IntRect::fromXYWH(left, top, static_cast<std::uint32_t>(width64),
                           static_cast<std::uint32_t>(height64));
}

void fillFixedRect(const FixedRect& rect, Blitter& blitter) {
  fillDot8(fdot8::fromFdot16(rect.left), fdot8::fromFdot16(rect.top), fdot8::fromFdot16(rect.right),
           fdot8::fromFdot16(rect.bottom), true, blitter);
}

void fillDot8(FDot8 l, FDot8 t, FDot8 r, FDot8 b, bool fillInner, Blitter& blitter) {
  auto toAlpha = [](std::int32_t alpha) { return static_cast<AlphaU8>(alpha); };

  if (l >= r || t >= b) {
    return;
  }

  auto top = t >> 8;
  if (top == ((b - 1) >> 8)) {
    doScanline(l, top, r, toAlpha(b - t - 1), blitter);
    return;
  }

  if ((t & 0xFF) != 0) {
    doScanline(l, top, r, toAlpha(256 - (t & 0xFF)), blitter);
    top += 1;
  }

  const auto bottom = b >> 8;
  const auto height = bottom - top;
  const auto heightOpt = toLengthU32(height);
  if (!heightOpt.has_value()) {
    return;
  }
  auto widthHeight = heightOpt.value();

  auto left = l >> 8;
  if (left == ((r - 1) >> 8)) {
    if (const auto leftX = toU32(left); const auto topY = toU32(top)) {
      blitter.blitV(leftX.value(), topY.value(), widthHeight, toAlpha(r - l - 1));
    }
    return;
  }

  if ((l & 0xFF) != 0) {
    if (const auto leftX = toU32(left); const auto topY = toU32(top)) {
      blitter.blitV(leftX.value(), topY.value(), widthHeight, toAlpha(256 - (l & 0xFF)));
    }
    left += 1;
  }

  const auto right = r >> 8;
  const auto innerWidth = right - left;
  if (fillInner) {
    if (const auto width = toLengthU32(innerWidth); width.has_value()) {
      if (const auto leftX = toU32(left); const auto topY = toU32(top)) {
        const auto rect =
            ScreenIntRect::fromXYWHSafe(leftX.value(), topY.value(), width.value(), widthHeight);
        blitter.blitRect(rect);
      }
    }
  }

  if ((r & 0xFF) != 0) {
    if (const auto rightX = toU32(right); const auto topY = toU32(top)) {
      blitter.blitV(rightX.value(), topY.value(), widthHeight, toAlpha(r & 0xFF));
    }
  }

  if ((b & 0xFF) != 0) {
    doScanline(l, bottom, r, toAlpha(b & 0xFF), blitter);
  }
}

void doScanline(FDot8 l, std::int32_t top, FDot8 r, AlphaU8 alpha, Blitter& blitter) {
  if (l >= r) {
    return;
  }

  auto yOpt = toU32(top);
  if (!yOpt.has_value()) {
    return;
  }
  const auto y = yOpt.value();

  const auto left = l >> 8;
  if (left == ((r - 1) >> 8)) {
    // Both edges fall within the same pixel column.
    if (auto leftX = toU32(left)) {
      blitter.blitV(leftX.value(), y, kLengthU32One, alphaMul(alpha, r - l));
    }
    return;
  }

  const auto right = r >> 8;

  auto x = left;
  if ((l & 0xFF) != 0) {
    if (auto x0 = toU32(x)) {
      blitter.blitV(x0.value(), y, kLengthU32One, alphaMul(alpha, 256 - (l & 0xFF)));
    }
    ++x;
  }

  const auto width = right - x;
  if (width > 0) {
    if (auto leftX = toU32(x)) {
      if (auto widthU32 = toLengthU32(width)) {
        callHlineBlitter(leftX.value(), std::make_optional(y), widthU32.value(), alpha, blitter);
      }
    }
  }

  if ((r & 0xFF) != 0) {
    if (auto x1 = toU32(right)) {
      blitter.blitV(x1.value(), y, kLengthU32One, alphaMul(alpha, r & 0xFF));
    }
  }
}

void callHlineBlitter(std::uint32_t x, const std::optional<std::uint32_t>& y, LengthU32 count,
                      AlphaU8 alpha, Blitter& blitter) {
  if (count == 0) {
    return;
  }

  constexpr std::size_t kHLineStackBuffer = 100;
  std::array<AlphaU8, kHLineStackBuffer> aa{};
  std::array<AlphaRun, kHLineStackBuffer + 1> runs{};

  auto remaining = count;
  while (remaining > 0) {
    aa[0] = alpha;

    auto n = static_cast<LengthU32>(std::min<std::uint32_t>(remaining, kHLineStackBuffer));
    runs[0] = static_cast<std::uint16_t>(n);
    runs[n] = std::nullopt;

    if (y.has_value()) {
      blitter.blitAntiH(x, y.value(), std::span<AlphaU8>{aa}, std::span<AlphaRun>{runs});
    }

    x += n;
    if (n >= remaining || remaining == 0) {
      break;
    }
    remaining -= n;
  }
}

enum class BlitterKind {
  HLine,
  Horish,
  VLine,
  Vertish,
};

void antiHairLineRgn(std::span<const Point> points, const ScreenIntRect* clip, Blitter& blitter);

class AntiHairBlitter {
 public:
  virtual ~AntiHairBlitter() = default;
  virtual FDot16 drawCap(std::uint32_t x, FDot16 fy, FDot16 slope, std::int32_t mod64) = 0;
  virtual FDot16 drawLine(std::uint32_t x, std::uint32_t stopX, FDot16 fy, FDot16 slope) = 0;
};

class HLineAntiHairBlitter final : public AntiHairBlitter {
 public:
  explicit HLineAntiHairBlitter(Blitter& blitter) : blitter_(blitter) {}

  FDot16 drawCap(std::uint32_t x, FDot16 fy, FDot16, std::int32_t mod64) override {
    fy += fdot16::half;
    if (fy < 0) {
      fy = 0;
    }

    const auto y = toU32(fy >> 16).value_or(0);
    const auto a = i32ToAlpha(fy >> 8);
    auto ma = fdot6::smallScale(a, mod64);
    if (ma != 0) {
      callHlineBlitter(x, std::make_optional(y), kLengthU32One, ma, blitter_);
    }

    ma = fdot6::smallScale(255 - a, mod64);
    if (ma != 0) {
      callHlineBlitter(x, y == 0 ? std::nullopt : std::make_optional(y - 1), kLengthU32One, ma,
                       blitter_);
    }

    return fy - fdot16::half;
  }

  FDot16 drawLine(std::uint32_t x, std::uint32_t stopX, FDot16 fy, FDot16) override {
    const auto width = stopX - x;
    if (width == 0) {
      return fy;
    }

    fy += fdot16::half;
    if (fy < 0) {
      fy = 0;
    }

    const auto y = toU32(fy >> 16).value_or(0);
    auto a = i32ToAlpha(fy >> 8);
    if (a != 0) {
      callHlineBlitter(x, std::make_optional(y), width, a, blitter_);
    }

    a = static_cast<AlphaU8>(255 - a);
    if (a != 0) {
      callHlineBlitter(x, y == 0 ? std::nullopt : std::make_optional(y - 1), width, a, blitter_);
    }
    return fy - fdot16::half;
  }

 private:
  Blitter& blitter_;
};

class HorishAntiHairBlitter final : public AntiHairBlitter {
 public:
  explicit HorishAntiHairBlitter(Blitter& blitter) : blitter_(blitter) {}

  FDot16 drawCap(std::uint32_t x, FDot16 fy, FDot16 dy, std::int32_t mod64) override {
    fy += fdot16::half;
    if (fy < 0) {
      fy = 0;
    }

    const auto lowerY = toU32(fy >> 16).value_or(0);
    const auto a = i32ToAlpha(fy >> 8);
    const auto a0 = fdot6::smallScale(255 - a, mod64);
    const auto a1 = fdot6::smallScale(a, mod64);
    blitter_.blitAntiV2(x, lowerY == 0 ? 0u : lowerY - 1, a0, a1);
    return fy + dy - fdot16::half;
  }

  FDot16 drawLine(std::uint32_t x, std::uint32_t stopX, FDot16 fy, FDot16 dy) override {
    if (x == stopX) {
      return fy;
    }

    fy += fdot16::half;
    while (true) {
      if (fy < 0) {
        fy = 0;
      }
      const auto lowerY = toU32(fy >> 16).value_or(0);
      const auto a = i32ToAlpha(fy >> 8);
      blitter_.blitAntiV2(x, lowerY == 0 ? 0u : lowerY - 1, static_cast<AlphaU8>(255 - a), a);

      fy += dy;
      ++x;
      if (x >= stopX) {
        break;
      }
    }

    return fy - fdot16::half;
  }

 private:
  Blitter& blitter_;
};

class VLineAntiHairBlitter final : public AntiHairBlitter {
 public:
  explicit VLineAntiHairBlitter(Blitter& blitter) : blitter_(blitter) {}

  FDot16 drawCap(std::uint32_t y, FDot16 fx, FDot16, std::int32_t mod64) override {
    fx += fdot16::half;
    if (fx < 0) {
      fx = 0;
    }

    const auto x = toU32(fx >> 16).value_or(0);
    const auto a = i32ToAlpha(fx >> 8);
    auto ma = fdot6::smallScale(a, mod64);
    if (ma != 0) {
      blitter_.blitV(x, y, kLengthU32One, ma);
    }

    ma = fdot6::smallScale(255 - a, mod64);
    if (ma != 0) {
      blitter_.blitV(x == 0 ? 0u : x - 1, y, kLengthU32One, ma);
    }

    return fx - fdot16::half;
  }

  FDot16 drawLine(std::uint32_t y, std::uint32_t stopY, FDot16 fx, FDot16 dx) override {
    const auto height = stopY - y;
    if (height == 0) {
      return fx;
    }

    fx += fdot16::half;
    if (fx < 0) {
      fx = 0;
    }
    const auto x = toU32(fx >> 16).value_or(0);
    auto a = i32ToAlpha(fx >> 8);
    if (a != 0) {
      blitter_.blitV(x, y, height, a);
    }

    a = static_cast<AlphaU8>(255 - a);
    if (a != 0) {
      blitter_.blitV(x == 0 ? 0u : x - 1, y, height, a);
    }

    return fx - fdot16::half;
  }

 private:
  Blitter& blitter_;
};

class VertishAntiHairBlitter final : public AntiHairBlitter {
 public:
  explicit VertishAntiHairBlitter(Blitter& blitter) : blitter_(blitter) {}

  FDot16 drawCap(std::uint32_t y, FDot16 fx, FDot16 dx, std::int32_t mod64) override {
    fx += fdot16::half;
    if (fx < 0) {
      fx = 0;
    }

    const auto x = toU32(fx >> 16).value_or(0);
    const auto a = i32ToAlpha(fx >> 8);
    blitter_.blitAntiH2(x == 0 ? 0u : x - 1, y, fdot6::smallScale(255 - a, mod64),
                        fdot6::smallScale(a, mod64));
    return fx + dx - fdot16::half;
  }

  FDot16 drawLine(std::uint32_t y, std::uint32_t stopY, FDot16 fx, FDot16 dx) override {
    if (y == stopY) {
      return fx;
    }

    fx += fdot16::half;
    while (true) {
      fx = std::max<FDot16>(fx, 0);
      const auto x = toU32(fx >> 16).value_or(0);
      const auto a = i32ToAlpha(fx >> 8);
      blitter_.blitAntiH2(x == 0 ? 0u : x - 1, y, static_cast<AlphaU8>(255 - a), a);
      fx += dx;
      ++y;
      if (y >= stopY) {
        break;
      }
    }
    return fx - fdot16::half;
  }

 private:
  Blitter& blitter_;
};

class RectClipBlitter final : public Blitter {
 public:
  RectClipBlitter(Blitter& blitter, ScreenIntRect clip) : blitter_(blitter), clip_(clip) {}

  void blitAntiH(std::uint32_t x, std::uint32_t y, std::span<AlphaU8> alpha,
                 std::span<AlphaRun> runs) override {
    if (!yInRect(y, clip_) || x >= clip_.right()) {
      return;
    }

    auto aa = alpha;
    auto run = runs;
    auto startX = static_cast<std::int32_t>(x);
    auto endX = startX + static_cast<std::int32_t>(computeAntiWidth(run));

    if (endX <= static_cast<std::int32_t>(clip_.left())) {
      return;
    }

    if (startX < static_cast<std::int32_t>(clip_.left())) {
      const auto dx = static_cast<std::int32_t>(static_cast<std::int32_t>(clip_.left()) - startX);
      AlphaRuns::breakAt(aa, run, dx);
      aa = aa.subspan(static_cast<std::size_t>(dx));
      run = run.subspan(static_cast<std::size_t>(dx));
      startX = static_cast<std::int32_t>(clip_.left());
      endX = startX + static_cast<std::int32_t>(computeAntiWidth(run));
    }

    if (endX > static_cast<std::int32_t>(clip_.right())) {
      const auto width =
          static_cast<std::size_t>(static_cast<std::int32_t>(clip_.right()) - startX);
      AlphaRuns::breakAt(aa, run, static_cast<std::int32_t>(width));
      run = run.subspan(0, width + 1);
      if (width < run.size()) {
        run[width] = std::nullopt;
      }
      endX = static_cast<std::int32_t>(clip_.right());
    }

    if (startX < endX) {
      blitter_.blitAntiH(static_cast<std::uint32_t>(startX), y, aa, run);
    }
  }

  void blitV(std::uint32_t x, std::uint32_t y, LengthU32 height, AlphaU8 alpha) override {
    if (!xInRect(x, clip_)) {
      return;
    }

    auto startY = static_cast<std::int32_t>(y);
    auto stopY = startY + static_cast<std::int32_t>(height);
    if (startY < static_cast<std::int32_t>(clip_.top())) {
      startY = static_cast<std::int32_t>(clip_.top());
    }
    if (stopY > static_cast<std::int32_t>(clip_.bottom())) {
      stopY = static_cast<std::int32_t>(clip_.bottom());
    }

    if (startY < stopY) {
      blitter_.blitV(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(startY),
                     static_cast<LengthU32>(stopY - startY), alpha);
    }
  }

  void blitAntiH2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) override {
    std::array<AlphaU8, 2> aa{alpha0, alpha1};
    std::array<AlphaRun, 3> runs{std::uint16_t(1), std::uint16_t(1), std::nullopt};
    blitAntiH(x, y, std::span<AlphaU8>{aa}, std::span<AlphaRun>{runs});
  }

  void blitAntiV2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) override {
    std::array<AlphaU8, 1> aa{alpha0};
    std::array<AlphaRun, 2> runs{std::uint16_t(1), std::nullopt};
    blitAntiH(x, y, std::span<AlphaU8>{aa}, std::span<AlphaRun>{runs});
    std::array<AlphaU8, 1> bb{alpha1};
    blitAntiH(x, y + 1, std::span<AlphaU8>{bb}, std::span<AlphaRun>{runs});
  }

 private:
  static bool xInRect(std::uint32_t x, const ScreenIntRect& rect) {
    return (x - rect.left()) < rect.width();
  }

  static bool yInRect(std::uint32_t y, const ScreenIntRect& rect) {
    return (y - rect.top()) < rect.height();
  }

  static std::uint32_t computeAntiWidth(std::span<AlphaRun> runs) {
    std::uint32_t width = 0;
    for (std::size_t i = 0; i < runs.size();) {
      if (!runs[i].has_value()) {
        break;
      }
      const auto run = runs[i].value();
      width += static_cast<std::uint32_t>(run);
      i += static_cast<std::size_t>(run);
    }
    return width;
  }

  Blitter& blitter_;
  ScreenIntRect clip_;
};

std::int32_t badInt(std::int32_t x) { return x & -x; }

std::int32_t anyBadInts(std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d) {
  return (badInt(a) | badInt(b) | badInt(c) | badInt(d)) >>
         (static_cast<std::int32_t>(sizeof(std::int32_t) << 3) - 1);
}

std::int32_t contribution64(std::int32_t ordinate) { return ((ordinate - 1) & 63) + 1; }

AlphaU8 i32ToAlpha(std::int32_t alpha) { return static_cast<AlphaU8>(alpha & 0xFF); }

void doAntiHairline(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                    std::optional<ScreenIntRect> clipOpt, Blitter& blitter) {
  if (anyBadInts(x0, y0, x1, y1) != 0) {
    return;
  }

  if ((x1 - x0) > fdot6::fromI32(511) || (y1 - y0) > fdot6::fromI32(511) ||
      (x0 - x1) > fdot6::fromI32(511) || (y0 - y1) > fdot6::fromI32(511)) {
    const auto hx = (x0 >> 1) + (x1 >> 1);
    const auto hy = (y0 >> 1) + (y1 >> 1);
    doAntiHairline(x0, y0, hx, hy, clipOpt, blitter);
    doAntiHairline(hx, hy, x1, y1, clipOpt, blitter);
    return;
  }

  std::int32_t scaleStart;
  std::int32_t scaleStop;
  std::int32_t iStart;
  std::int32_t iStop;
  FDot16 fStart;
  FDot16 slope;
  BlitterKind blitterKind;

  if (std::abs(x1 - x0) > std::abs(y1 - y0)) {
    if (x0 > x1) {
      std::swap(x0, x1);
      std::swap(y0, y1);
    }

    iStart = fdot6::floor(x0);
    iStop = fdot6::ceil(x1);
    fStart = fdot6::toFdot16(y0);
    if (y0 == y1) {
      slope = 0;
      blitterKind = BlitterKind::HLine;
    } else {
      slope = fdot16::fastDiv(y1 - y0, x1 - x0);
      fStart += (static_cast<std::int64_t>(slope) * (32 - (x0 & 63)) + 32) >> 6;
      blitterKind = BlitterKind::Horish;
    }

    if (iStop - iStart == 1) {
      scaleStart = x1 - x0;
      scaleStop = 0;
    } else {
      scaleStart = 64 - (x0 & 63);
      scaleStop = x1 & 63;
    }

    if (clipOpt.has_value()) {
      const auto clip = clipOpt->toIntRect();
      if (iStart >= clip.right() || iStop <= clip.left()) {
        return;
      }

      if (iStart < clip.left()) {
        fStart += static_cast<std::int64_t>(slope) * (clip.left() - iStart);
        iStart = clip.left();
        scaleStart = 64;
        if (iStop - iStart == 1) {
          scaleStart = contribution64(x1);
          scaleStop = 0;
        }
      }

      if (iStop > clip.right()) {
        iStop = clip.right();
        scaleStop = 0;
      }

      if (iStart >= iStop) {
        return;
      }

      std::int32_t top, bottom;
      if (slope >= 0) {
        // T2B
        top = fdot16::floorToI32(fStart - fdot16::half);
        bottom = fdot16::ceilToI32(fStart + static_cast<std::int64_t>(iStop - iStart - 1) * slope +
                                   fdot16::half);
      } else {
        // B2T
        bottom = fdot16::ceilToI32(fStart + fdot16::half);
        top = fdot16::floorToI32(fStart + static_cast<std::int64_t>(iStop - iStart - 1) * slope -
                                 fdot16::half);
      }
      top -= 1;
      bottom += 1;

      if (top >= clip.bottom() || bottom <= clip.top()) {
        return;
      }
      if (clip.top() <= top && clip.bottom() >= bottom) {
        clipOpt.reset();
      }
    }
  } else {
    if (y0 > y1) {
      std::swap(x0, x1);
      std::swap(y0, y1);
    }

    iStart = fdot6::floor(y0);
    iStop = fdot6::ceil(y1);
    fStart = fdot6::toFdot16(x0);
    if (x0 == x1) {
      if (y0 == y1) {
        return;
      }
      slope = 0;
      blitterKind = BlitterKind::VLine;
    } else {
      slope = fdot16::fastDiv(x1 - x0, y1 - y0);
      fStart += (static_cast<std::int64_t>(slope) * (32 - (y0 & 63)) + 32) >> 6;
      blitterKind = BlitterKind::Vertish;
    }

    if (iStop - iStart == 1) {
      scaleStart = y1 - y0;
      scaleStop = 0;
    } else {
      scaleStart = 64 - (y0 & 63);
      scaleStop = y1 & 63;
    }

    if (clipOpt.has_value()) {
      const auto clip = clipOpt->toIntRect();
      if (iStart >= clip.bottom() || iStop <= clip.top()) {
        return;
      }

      if (iStart < clip.top()) {
        fStart += static_cast<std::int64_t>(slope) * (clip.top() - iStart);
        iStart = clip.top();
        scaleStart = 64;
        if (iStop - iStart == 1) {
          scaleStart = contribution64(y1);
          scaleStop = 0;
        }
      }
      if (iStop > clip.bottom()) {
        iStop = clip.bottom();
        scaleStop = 0;
      }
      if (iStart >= iStop) {
        return;
      }

      std::int32_t left, right;
      if (slope >= 0) {
        // L2R
        left = fdot16::floorToI32(fStart - fdot16::half);
        right = fdot16::ceilToI32(fStart + static_cast<std::int64_t>(iStop - iStart - 1) * slope +
                                  fdot16::half);
      } else {
        // R2L
        right = fdot16::ceilToI32(fStart + fdot16::half);
        left = fdot16::floorToI32(fStart + static_cast<std::int64_t>(iStop - iStart - 1) * slope -
                                  fdot16::half);
      }
      left -= 1;
      right += 1;

      if (left >= clip.right() || right <= clip.left()) {
        return;
      }
      if (clip.left() <= left && clip.right() >= right) {
        clipOpt.reset();
      }
    }
  }

  std::optional<RectClipBlitter> clipBlitter;
  Blitter* drawBlitter = &blitter;
  if (clipOpt.has_value()) {
    clipBlitter.emplace(blitter, clipOpt.value());
    drawBlitter = &clipBlitter.value();
  }

  AntiHairBlitter* antiBlitter = nullptr;
  HLineAntiHairBlitter hLine(*drawBlitter);
  HorishAntiHairBlitter horish(*drawBlitter);
  VLineAntiHairBlitter vLine(*drawBlitter);
  VertishAntiHairBlitter vertish(*drawBlitter);

  switch (blitterKind) {
    case BlitterKind::HLine:
      antiBlitter = &hLine;
      break;
    case BlitterKind::Horish:
      antiBlitter = &horish;
      break;
    case BlitterKind::VLine:
      antiBlitter = &vLine;
      break;
    case BlitterKind::Vertish:
      antiBlitter = &vertish;
      break;
  }
  if (antiBlitter == nullptr) {
    return;
  }

  if (iStart < 0 || iStop < 0) {
    return;
  }

  fStart = antiBlitter->drawCap(static_cast<std::uint32_t>(iStart), fStart, slope, scaleStart);
  iStart += 1;

  const auto fullSpans = iStop - iStart - (scaleStop > 0 ? 1 : 0);
  if (fullSpans > 0) {
    fStart = antiBlitter->drawLine(static_cast<std::uint32_t>(iStart),
                                   static_cast<std::uint32_t>(iStart + fullSpans), fStart, slope);
  }
  if (scaleStop > 0) {
    antiBlitter->drawCap(static_cast<std::uint32_t>(iStop - 1), fStart, slope, scaleStop);
  }
}

void antiHairLineRgn(std::span<const Point> points, const ScreenIntRect* clip, Blitter& blitter) {
  const auto fixedBounds = Rect::fromLTRB(-kMaxCoord, -kMaxCoord, kMaxCoord, kMaxCoord);
  if (!fixedBounds.has_value()) {
    return;
  }

  std::optional<Rect> clipBounds;
  if (clip != nullptr) {
    const auto clipRect = clip->toRect();
    clipBounds = Rect::fromLTRB(clipRect.left() - 1.0f, clipRect.top() - 1.0f,
                                clipRect.right() + 1.0f, clipRect.bottom() + 1.0f);
    if (!clipBounds.has_value()) {
      return;
    }
  }

  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    std::array<Point, 2> segment{points[i], points[i + 1]};
    std::array<Point, 2> clipped{};
    if (!lineClipper::intersect(std::span<const Point, 2>{segment}, fixedBounds.value(),
                                 std::span<Point, 2>{clipped})) {
      continue;
    }

    auto working = clipped;
    if (clipBounds.has_value()) {
      std::array<Point, 2> clipClipped{};
      if (!lineClipper::intersect(std::span<const Point, 2>{working}, clipBounds.value(),
                                   std::span<Point, 2>{clipClipped})) {
        continue;
      }
      working = clipClipped;
    }

    const auto x0 = fdot6::fromF32(working[0].x);
    const auto y0 = fdot6::fromF32(working[0].y);
    const auto x1 = fdot6::fromF32(working[1].x);
    const auto y1 = fdot6::fromF32(working[1].y);

    if (clip != nullptr) {
      const auto left = std::min(x0, x1);
      const auto top = std::min(y0, y1);
      const auto right = std::max(x0, x1);
      const auto bottom = std::max(y0, y1);

      const auto ir = makeIntRectFromLTRB(fdot6::floor(left) - 1, fdot6::floor(top) - 1,
                                          fdot6::ceil(right) + 1, fdot6::ceil(bottom) + 1);
      if (!ir.has_value()) {
        return;
      }

      const auto clipInt = clip->toIntRect();
      if (!ir.value().intersect(clipInt).has_value()) {
        continue;
      }

      if (!(clipInt.left() <= ir->left() && clipInt.top() <= ir->top() &&
            clipInt.right() >= ir->right() && clipInt.bottom() >= ir->bottom())) {
        if (auto sub = ir->intersect(clipInt); sub.has_value()) {
          if (auto subclip = sub->toScreenIntRect(); subclip.has_value()) {
            doAntiHairline(x0, y0, x1, y1, subclip, blitter);
          }
        }
        continue;
      }
    }

    doAntiHairline(x0, y0, x1, y1, std::nullopt, blitter);
  }
}

}  // namespace

namespace scan::hairline_aa {

void fillRect(const Rect& rect, const ScreenIntRect& clip, Blitter& blitter) {
  const auto clipRect = clip.toRect();
  const auto clippedRect = Rect::fromLTRB(
      std::max(rect.left(), clipRect.left()), std::max(rect.top(), clipRect.top()),
      std::min(rect.right(), clipRect.right()), std::min(rect.bottom(), clipRect.bottom()));
  if (!clippedRect.has_value()) {
    return;
  }

  if (const auto fixedRect = FixedRect::fromRect(clippedRect.value())) {
    fillFixedRect(fixedRect.value(), blitter);
  }
}

void strokePath(const Path& path, LineCap lineCap, const ScreenIntRect& clip, Blitter& blitter) {
  tiny_skia::scan::strokePathImpl(path, lineCap, clip, antiHairLineRgn, blitter);
}

}  // namespace scan::hairline_aa

}  // namespace tiny_skia
