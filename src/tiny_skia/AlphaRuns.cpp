#include "tiny_skia/AlphaRuns.h"

#include <limits>

namespace tiny_skia {

namespace {

AlphaRun makeRun(std::size_t value) {
  assert(value <= std::numeric_limits<std::uint16_t>::max());
  if (value == 0) {
    return std::nullopt;
  }
  return AlphaRun{static_cast<std::uint16_t>(value)};
}

}  // namespace

AlphaRuns::AlphaRuns(LengthU32 width) : runs(width + 1), alpha(width + 1, 0) { reset(width); }

std::uint8_t AlphaRuns::catchOverflow(std::uint16_t alpha) {
  assert(alpha <= 256);
  return static_cast<std::uint8_t>(alpha - (alpha >> 8));
}

bool AlphaRuns::empty() const {
  assert(runs[0].has_value());
  if (const auto run = runs[0]; run.has_value()) {
    return alpha[0] == 0 && !runs[static_cast<std::size_t>(run.value())].has_value();
  }
  return true;
}

void AlphaRuns::reset(LengthU32 width) {
  assert(width > 0);
  assert(width <= std::numeric_limits<std::uint16_t>::max());
  const auto run = static_cast<std::size_t>(width);

  if (runs.size() < static_cast<std::size_t>(width) + 1) {
    runs.resize(width + 1);
    alpha.resize(width + 1);
  }

  runs[0] = makeRun(run);
  runs[static_cast<std::size_t>(width)] = std::nullopt;
  alpha[0] = 0;
}

std::size_t AlphaRuns::add(std::uint32_t x, AlphaU8 startAlpha, std::size_t middleCount,
                           AlphaU8 stopAlpha, std::uint8_t maxValue, std::size_t offsetX) {
  std::size_t xUsize = static_cast<std::size_t>(x);

  std::size_t runsOffset = offsetX;
  std::size_t alphaOffset = offsetX;
  std::size_t lastAlphaOffset = offsetX;
  assert(xUsize >= offsetX);
  xUsize -= offsetX;

  if (startAlpha != 0) {
    breakRun(std::span{runs}.subspan(runsOffset), std::span{alpha}.subspan(alphaOffset), xUsize,
             1);

    const std::uint16_t tmp =
        static_cast<std::uint16_t>(alpha[alphaOffset + xUsize] + startAlpha);
    assert(tmp <= 256);
    alpha[alphaOffset + xUsize] = catchOverflow(tmp);

    runsOffset += xUsize + 1;
    alphaOffset += xUsize + 1;
    xUsize = 0;
  }

  if (middleCount != 0) {
    breakRun(std::span{runs}.subspan(runsOffset), std::span{alpha}.subspan(alphaOffset), xUsize,
             middleCount);

    alphaOffset += xUsize;
    runsOffset += xUsize;
    xUsize = 0;
    do {
      alpha[alphaOffset] = catchOverflow(
          static_cast<std::uint16_t>(static_cast<std::uint16_t>(alpha[alphaOffset]) + maxValue));
      const std::size_t n = static_cast<std::size_t>(runs[runsOffset].value());
      assert(n <= middleCount);
      alphaOffset += n;
      runsOffset += n;
      middleCount -= n;
      if (middleCount == 0) {
        break;
      }
    } while (true);

    lastAlphaOffset = alphaOffset;
  }

  if (stopAlpha != 0) {
    breakRun(std::span{runs}.subspan(runsOffset), std::span{alpha}.subspan(alphaOffset), xUsize,
             1);
    alphaOffset += xUsize;
    alpha[alphaOffset] += stopAlpha;
    lastAlphaOffset = alphaOffset;
  }

  return lastAlphaOffset;
}

void AlphaRuns::breakRun(std::span<AlphaRun> runs, std::span<std::uint8_t> alpha, std::size_t x,
                         std::size_t count) {
  assert(count > 0);

  const std::size_t origX = x;
  std::size_t runsOffset = 0;
  std::size_t alphaOffset = 0;

  while (x > 0) {
    const std::size_t n = static_cast<std::size_t>(runs[runsOffset].value());
    assert(n > 0);

    if (x < n) {
      alpha[alphaOffset + x] = alpha[alphaOffset];
      runs[runsOffset] = makeRun(x);
      runs[runsOffset + x] = makeRun(n - x);
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
    const std::size_t n = static_cast<std::size_t>(runs[runsOffset].value());
    assert(n > 0);

    if (x < n) {
      alpha[alphaOffset + x] = alpha[alphaOffset];
      runs[runsOffset] = makeRun(x);
      runs[runsOffset + x] = makeRun(n - x);
      break;
    }

    x -= n;
    if (x == 0) {
      break;
    }

    runsOffset += n;
    alphaOffset += n;
  }
}

void AlphaRuns::breakAt(std::span<std::uint8_t> alpha, std::span<AlphaRun> runs, std::int32_t x) {
  std::size_t alphaI = 0;
  std::size_t runI = 0;
  while (x > 0) {
    const auto n = runs[runI].value();
    const std::size_t nUsize = static_cast<std::size_t>(n);
    const std::int32_t nI32 = static_cast<std::int32_t>(n);
    if (x < nI32) {
      alpha[alphaI + static_cast<std::size_t>(x)] = alpha[alphaI];
      runs[0] = makeRun(static_cast<std::size_t>(x));
      runs[static_cast<std::size_t>(x)] = makeRun(static_cast<std::size_t>(nI32 - x));
      break;
    }

    runI += nUsize;
    alphaI += nUsize;
    x -= nI32;
  }
}

}  // namespace tiny_skia
