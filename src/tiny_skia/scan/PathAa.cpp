#include "tiny_skia/scan/PathAa.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "tiny_skia/EdgeBuilder.h"
#include "tiny_skia/Math.h"
#include "tiny_skia/scan/Path.h"

namespace tiny_skia {

namespace {

constexpr std::uint32_t kSuperSampleShift = 2;
constexpr std::uint32_t kScale = 1u << kSuperSampleShift;
constexpr std::uint32_t kMask = kScale - 1;

std::int32_t overflowsShortShift(std::int32_t value, std::int32_t shift) {
  return (leftShift(value, 16 + shift) >> (16 + shift)) - value;
}

std::int32_t rectOverflowsShortShift(const IntRect& rect, std::int32_t shift) {
  return overflowsShortShift(rect.left(), shift) | overflowsShortShift(rect.top(), shift) |
         overflowsShortShift(rect.right(), shift) | overflowsShortShift(rect.bottom(), shift);
}

AlphaU8 coverageToPartialAlpha(std::uint32_t aa) {
  return static_cast<AlphaU8>(aa << (8 - 2 * kSuperSampleShift));
}

struct BaseSuperBlitter {
  Blitter* realBlitter = nullptr;
  std::int32_t currIy = 0;
  std::uint32_t width = 0;
  std::uint32_t left = 0;
  std::uint32_t superLeft = 0;
  std::int32_t currY = 0;
  std::int32_t top = 0;

  static std::optional<BaseSuperBlitter> create(const IntRect& bounds,
                                                const ScreenIntRect& clipRect,
                                                Blitter& realBlitter) {
    const auto boundsIntersection = bounds.intersect(clipRect.toIntRect());
    if (!boundsIntersection) {
      return std::nullopt;
    }

    const auto clipped = boundsIntersection->toScreenIntRect();
    if (!clipped) {
      return std::nullopt;
    }

    return BaseSuperBlitter{&realBlitter,
                            static_cast<std::int32_t>(clipped->top()) - 1,
                            clipped->width(),
                            clipped->left(),
                            clipped->left() << kSuperSampleShift,
                            static_cast<std::int32_t>(clipped->top() << kSuperSampleShift) - 1,
                            static_cast<std::int32_t>(clipped->top())};
  }
};

struct SuperBlitter final : public Blitter {
  explicit SuperBlitter(BaseSuperBlitter base) : base_(std::move(base)), runs_(base_.width) {}

  ~SuperBlitter() override { flush(); }

  void blitH(std::uint32_t x, std::uint32_t y, LengthU32 width) override {
    const auto iy = static_cast<std::int32_t>(y >> kSuperSampleShift);
    if (x < base_.superLeft) {
      width += x;
      x = 0;
    } else {
      x -= base_.superLeft;
    }

    // Clamp right edge to prevent out-of-bounds access in AlphaRuns.
    const auto maxSuperWidth = base_.width << kSuperSampleShift;
    if (x >= maxSuperWidth) {
      return;
    }
    if (x + width > maxSuperWidth) {
      width = maxSuperWidth - x;
    }
    if (width == 0) {
      return;
    }

    if (static_cast<std::int32_t>(y) != base_.currY) {
      offsetX_ = 0;
      base_.currY = static_cast<std::int32_t>(y);
    }

    if (iy != base_.currIy) {
      flush();
      base_.currIy = iy;
    }

    const auto start = x;
    const auto stop = x + width;

    std::uint32_t fb = start & kMask;
    std::uint32_t fe = stop & kMask;
    std::int32_t n = (static_cast<std::int32_t>(stop >> kSuperSampleShift)) -
                     (static_cast<std::int32_t>(start >> kSuperSampleShift)) - 1;

    if (n < 0) {
      fb = fe - fb;
      n = 0;
      fe = 0;
    } else {
      if (fb == 0) {
        ++n;
      } else {
        fb = kScale - fb;
      }
    }

    const auto maxValue = static_cast<AlphaU8>((1 << (8 - kSuperSampleShift)) -
                                               (((y & kMask) + 1) >> kSuperSampleShift));
    offsetX_ =
        runs_.add(x >> kSuperSampleShift, coverageToPartialAlpha(fb), static_cast<std::size_t>(n),
                  coverageToPartialAlpha(fe), maxValue, offsetX_);
  }

 private:
  void flush() {
    if (base_.currIy >= base_.top && !runs_.empty()) {
      base_.realBlitter->blitAntiH(base_.left, static_cast<std::uint32_t>(base_.currIy),
                                   runs_.alpha, runs_.runs);
      runs_.reset(base_.width);
      offsetX_ = 0;
      base_.currIy = base_.top - 1;
    }
  }

  BaseSuperBlitter base_;
  AlphaRuns runs_;
  std::size_t offsetX_ = 0;
};

}  // namespace

namespace scan::path_aa {

void fillPath(const Path& path, FillRule fillRule, const ScreenIntRect& clip, Blitter& blitter) {
  const auto boundsOpt = path.bounds().roundOut();
  if (!boundsOpt) {
    return;
  }

  const auto clipped = boundsOpt->intersect(clip.toIntRect());
  if (!clipped) {
    return;
  }

  if (rectOverflowsShortShift(clipped.value(), static_cast<std::int32_t>(kSuperSampleShift)) != 0) {
    tiny_skia::scan::fillPath(path, fillRule, clip, blitter);
    return;
  }

  if (clip.right() > 32767 || clip.bottom() > 32767) {
    return;
  }

  const auto pathContainedInClip = [&]() {
    const auto boundsScreen = boundsOpt->toScreenIntRect();
    return boundsScreen.has_value() && clip.contains(boundsScreen.value());
  }();

  fillPathImpl(path, fillRule, *boundsOpt, clip, static_cast<std::int32_t>(boundsOpt->y()),
               static_cast<std::int32_t>(boundsOpt->y() + boundsOpt->height()),
               static_cast<std::int32_t>(kSuperSampleShift), pathContainedInClip, blitter);
}

void fillPathImpl(const Path& path, FillRule fillRule, const IntRect& bounds,
                  const ScreenIntRect& clipRect, std::int32_t startY, std::int32_t stopY,
                  std::int32_t shiftEdgesUp, bool pathContainedInClip, Blitter& blitter) {
  const auto baseOpt = BaseSuperBlitter::create(bounds, clipRect, blitter);
  if (!baseOpt) {
    return;
  }

  SuperBlitter superBlitter(baseOpt.value());
  tiny_skia::scan::fillPathImpl(path, fillRule, clipRect, startY, stopY, shiftEdgesUp,
                                pathContainedInClip, superBlitter);
}

}  // namespace scan::path_aa

}  // namespace tiny_skia
