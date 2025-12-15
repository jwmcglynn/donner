#include "donner/backends/tiny_skia_cpp/AlphaRuns.h"

#include <algorithm>

namespace donner::backends::tiny_skia_cpp {

namespace {
constexpr uint8_t kMaxCoverage = 255;
}

AlphaRuns::AlphaRuns(uint32_t width)
    : runs_(static_cast<size_t>(width) + 1), alpha_(static_cast<size_t>(width) + 1) {
  reset(width);
}

bool AlphaRuns::isEmpty() const {
  if (runs_[0] == 0) {
    return true;
  }

  const uint16_t firstRun = runs_[0];
  const size_t sentinelIndex = static_cast<size_t>(firstRun);
  return alpha_[0] == 0 && sentinelIndex < runs_.size() && runs_[sentinelIndex] == 0;
}

void AlphaRuns::reset(uint32_t width) {
  const uint16_t run = static_cast<uint16_t>(width);
  runs_.assign(static_cast<size_t>(width) + 1, 0);
  alpha_.assign(static_cast<size_t>(width) + 1, 0);
  runs_[0] = run;
  runs_[width] = 0;
  alpha_[0] = 0;
}

size_t AlphaRuns::add(uint32_t x, uint8_t startAlpha, size_t middleCount, uint8_t stopAlpha,
                      uint8_t maxValue, size_t offsetX) {
  size_t localX = static_cast<size_t>(x);
  const size_t clampedOffset = std::min(offsetX, localX);
  size_t runsOffset = clampedOffset;
  size_t alphaOffset = clampedOffset;
  size_t lastAlphaOffset = clampedOffset;

  localX -= clampedOffset;

  if (startAlpha != 0) {
    breakRun({runs_.data() + runsOffset, runs_.size() - runsOffset},
             {alpha_.data() + alphaOffset, alpha_.size() - alphaOffset}, localX, 1);

    const uint16_t tmp =
        static_cast<uint16_t>(alpha_[alphaOffset + localX]) + static_cast<uint16_t>(startAlpha);
    alpha_[alphaOffset + localX] = static_cast<uint8_t>(tmp - (tmp >> 8));

    runsOffset += localX + 1;
    alphaOffset += localX + 1;
    localX = 0;
  }

  if (middleCount != 0) {
    breakRun({runs_.data() + runsOffset, runs_.size() - runsOffset},
             {alpha_.data() + alphaOffset, alpha_.size() - alphaOffset}, localX, middleCount);

    alphaOffset += localX;
    runsOffset += localX;
    localX = 0;

    while (middleCount != 0) {
      const uint8_t a = catchOverflow(static_cast<uint16_t>(alpha_[alphaOffset]) +
                                      static_cast<uint16_t>(maxValue));
      alpha_[alphaOffset] = a;

      const size_t n = runs_[runsOffset];
      if (n == 0) {
        break;
      }
      alphaOffset += n;
      runsOffset += n;
      middleCount -= n;
    }

    lastAlphaOffset = alphaOffset;
  }

  if (stopAlpha != 0) {
    breakRun({runs_.data() + runsOffset, runs_.size() - runsOffset},
             {alpha_.data() + alphaOffset, alpha_.size() - alphaOffset}, localX, 1);

    alphaOffset += localX;
    alpha_[alphaOffset] = std::min<uint16_t>(kMaxCoverage, alpha_[alphaOffset] + stopAlpha);
    lastAlphaOffset = alphaOffset;
  }

  return lastAlphaOffset;
}

uint8_t AlphaRuns::catchOverflow(uint16_t alpha) {
  return static_cast<uint8_t>(alpha - (alpha >> 8));
}

void AlphaRuns::breakRun(std::span<uint16_t> runs, std::span<uint8_t> alpha, size_t x,
                         size_t count) {
  size_t origX = x;
  size_t runsOffset = 0;
  size_t alphaOffset = 0;

  while (x > 0) {
    const size_t n = runs[runsOffset];
    if (n == 0) {
      break;
    }
    if (x < n) {
      alpha[alphaOffset + x] = alpha[alphaOffset];
      runs[runsOffset] = static_cast<uint16_t>(x);
      runs[runsOffset + x] = static_cast<uint16_t>(n - x);
      break;
    }

    runsOffset += n;
    alphaOffset += n;
    x -= n;
  }

  runsOffset = origX;
  alphaOffset = origX;
  x = count;

  while (true) {
    const size_t n = runs[runsOffset];
    if (n == 0) {
      break;
    }
    if (x < n) {
      alpha[alphaOffset + x] = alpha[alphaOffset];
      runs[runsOffset] = static_cast<uint16_t>(x);
      runs[runsOffset + x] = static_cast<uint16_t>(n - x);
      break;
    }

    x -= n;
    if (x == 0) {
      break;
    }

    alphaOffset += n;
    runsOffset += n;
  }
}

}  // namespace donner::backends::tiny_skia_cpp
