// Analytic Anti-Aliasing (AAA) path fill implementation.
// Ported from Skia's SkScan_AAAPath.cpp for bit-exact coverage computation.
// Original copyright: Copyright 2016 The Android Open Source Project (BSD license).

#include "tiny_skia/scan/PathAa.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "tiny_skia/AnalyticEdge.h"
#include "tiny_skia/EdgeBuilder.h"
#include "tiny_skia/EdgeClipper.h"
#include "tiny_skia/Math.h"
#include "tiny_skia/PathGeometry.h"
#include "tiny_skia/scan/Path.h"

namespace tiny_skia {

namespace {

// ── Fixed-point helpers (matching Skia's conventions) ────────────────────────

FDot16 fixedCeilToFixed(FDot16 x) { return (x + fdot16::one - 1) & ~0xFFFF; }
FDot16 fixedFloorToFixed(FDot16 x) { return x & ~0xFFFF; }
FDot16 intToFixed(std::int32_t x) { return x << 16; }
FDot6 fixedToFDot6(FDot16 x) { return x >> 10; }

std::int32_t sat_add(std::int32_t a, std::int32_t b) {
  auto r = static_cast<std::int64_t>(a) + b;
  if (r > std::numeric_limits<std::int32_t>::max()) {
    return std::numeric_limits<std::int32_t>::max();
  }
  if (r < std::numeric_limits<std::int32_t>::min()) {
    return std::numeric_limits<std::int32_t>::min();
  }
  return static_cast<std::int32_t>(r);
}

// ── Coverage computation helpers ─────────────────────────────────────────────

AlphaU8 trapezoidToAlpha(FDot16 l1, FDot16 l2) {
  assert(l1 >= 0 && l2 >= 0);
  FDot16 area = (l1 + l2) / 2;
  return static_cast<AlphaU8>(area >> 8);
}

AlphaU8 partialTriangleToAlpha(FDot16 a, FDot16 b) {
  assert(a <= fdot16::one);
  FDot16 area = (a >> 11) * (a >> 11) * (b >> 11);
  return static_cast<AlphaU8>((area >> 8) & 0xFF);
}

AlphaU8 getPartialAlpha(AlphaU8 alpha, FDot16 partialHeight) {
  return static_cast<AlphaU8>(fdot16::roundToI32(alpha * partialHeight));
}

AlphaU8 getPartialAlphaU8(AlphaU8 alpha, AlphaU8 fullAlpha) {
  return static_cast<AlphaU8>((alpha * fullAlpha) >> 8);
}

AlphaU8 fixedToAlpha(FDot16 f) {
  assert(f <= fdot16::one);
  return getPartialAlpha(0xFF, f);
}

FDot16 approximateIntersection(FDot16 l1, FDot16 r1, FDot16 l2, FDot16 r2) {
  if (l1 > r1) std::swap(l1, r1);
  if (l2 > r2) std::swap(l2, r2);
  return (std::max(l1, l2) + std::min(r1, r2)) / 2;
}

void safelyAddAlpha(AlphaU8& alpha, AlphaU8 delta) {
  alpha = static_cast<AlphaU8>(std::min(0xFF, static_cast<int>(alpha) + delta));
}

AlphaU8 snapAlpha(AlphaU8 alpha) {
  return alpha > 247 ? 0xFF : alpha < 8 ? 0x00 : alpha;
}

// ── Row-based additive blitter ───────────────────────────────────────────────
// Accumulates alpha values in a per-row buffer, flushing to the real blitter
// when moving to a new scanline. Matches Skia's RunBasedAdditiveBlitter semantics.

class AdditiveBlitter {
 public:
  // Skia's MaskAdditiveBlitter thresholds for small paths.
  static constexpr int kMaskMaxWidth = 32;
  static constexpr int kMaskMaxStorage = 1024;

  AdditiveBlitter(Blitter& realBlitter, std::int32_t left, std::int32_t width, std::int32_t top,
                  std::int32_t height)
      : realBlitter_(realBlitter),
        left_(left),
        width_(width),
        top_(top),
        currY_(top - 1),
        dirtyMin_(width),
        dirtyMax_(-1),
        row_(static_cast<std::size_t>(width) + 2, 0),
        // Match Skia: small paths use MaskAdditiveBlitter which doesn't snap alpha.
        useMaskMode_(width <= kMaskMaxWidth &&
                     static_cast<std::int64_t>(width) * height <= kMaskMaxStorage) {}

  ~AdditiveBlitter() { flush(); }

  Blitter& getRealBlitter() { return realBlitter_; }
  int getWidth() const { return width_; }

  void blitAntiH(int x, int y, AlphaU8 alpha) {
    if (alpha == 0) return;
    checkY(y);
    x -= left_;
    if (x >= 0 && x < width_) {
      safelyAddAlpha(row_[static_cast<std::size_t>(x)], alpha);
      if (x < dirtyMin_) dirtyMin_ = x;
      if (x > dirtyMax_) dirtyMax_ = x;
    }
  }

  void blitAntiH(int x, int y, int w, AlphaU8 alpha) {
    if (alpha == 0 || w <= 0) return;
    checkY(y);
    x -= left_;
    int start = std::max(0, x);
    int end = std::min(x + w, width_);
    for (int xi = start; xi < end; ++xi) {
      safelyAddAlpha(row_[static_cast<std::size_t>(xi)], alpha);
    }
    if (start < end) {
      if (start < dirtyMin_) dirtyMin_ = start;
      if (end - 1 > dirtyMax_) dirtyMax_ = end - 1;
    }
  }

  void blitAntiHArray(int x, int y, const AlphaU8* alphas, int len) {
    checkY(y);
    x -= left_;
    int start = std::max(0, -x);
    int end = std::min(len, width_ - x);
    for (int i = start; i < end; ++i) {
      safelyAddAlpha(row_[static_cast<std::size_t>(x + i)], alphas[i]);
    }
    if (start < end) {
      int lo = x + start;
      int hi = x + end - 1;
      if (lo < dirtyMin_) dirtyMin_ = lo;
      if (hi > dirtyMax_) dirtyMax_ = hi;
    }
  }

  void flushIfYChanged(FDot16 y, FDot16 nextY) {
    if (fdot16::floorToI32(y) != fdot16::floorToI32(nextY)) {
      flush();
    }
  }

 private:
  void checkY(int y) {
    if (y != currY_) {
      flush();
      currY_ = y;
    }
  }

  void flush() {
    if (currY_ < top_ || dirtyMax_ < dirtyMin_) {
      currY_ = top_ - 1;
      dirtyMin_ = width_;
      dirtyMax_ = -1;
      return;
    }

    auto uy = static_cast<std::uint32_t>(currY_);
    int i = dirtyMin_;
    int end = dirtyMax_ + 1;

    while (i < end) {
      AlphaU8 a = row_[static_cast<std::size_t>(i)];
      if (!useMaskMode_) {
        a = snapAlpha(a);
      }

      if (a == 0) {
        ++i;
        continue;
      }

      if (a == 0xFF) {
        int start = i;
        ++i;
        while (i < end) {
          AlphaU8 next = row_[static_cast<std::size_t>(i)];
          if (!useMaskMode_) next = snapAlpha(next);
          if (next != 0xFF) break;
          ++i;
        }
        realBlitter_.blitH(static_cast<std::uint32_t>(left_ + start), uy,
                           static_cast<LengthU32>(i - start));
      } else {
        realBlitter_.blitAntiH2(static_cast<std::uint32_t>(left_ + i), uy, a, 0);
        ++i;
      }
    }

    std::fill(row_.begin() + dirtyMin_, row_.begin() + end, static_cast<AlphaU8>(0));
    dirtyMin_ = width_;
    dirtyMax_ = -1;
    currY_ = top_ - 1;
  }

  Blitter& realBlitter_;
  std::int32_t left_;
  std::int32_t width_;
  std::int32_t top_;
  std::int32_t currY_;
  int dirtyMin_;
  int dirtyMax_;
  std::vector<AlphaU8> row_;
  bool useMaskMode_;  ///< When true, skip snapAlpha (matches Skia's MaskAdditiveBlitter).
};

// ── Trapezoid row blitting ───────────────────────────────────────────────────
// These functions compute exact geometric coverage for a trapezoid spanning
// one pixel row, matching Skia's blit_trapezoid_row and helpers.

void computeAlphaAboveLine(AlphaU8* alphas, FDot16 l, FDot16 r, FDot16 dY, AlphaU8 fullAlpha) {
  assert(l <= r);
  assert(l >> 16 == 0);
  int R = fdot16::ceilToI32(r);
  if (R == 0) {
    return;
  }
  if (R == 1) {
    alphas[0] = getPartialAlphaU8(((R << 17) - l - r) >> 9, fullAlpha);
  } else {
    FDot16 first = fdot16::one - l;
    FDot16 last = r - ((R - 1) << 16);
    FDot16 firstH = fdot16::mul(first, dY);
    alphas[0] = static_cast<AlphaU8>(fdot16::mul(first, firstH) >> 9);
    FDot16 alpha16 = sat_add(firstH, dY >> 1);
    for (int i = 1; i < R - 1; ++i) {
      alphas[i] = static_cast<AlphaU8>(alpha16 >> 8);
      alpha16 = sat_add(alpha16, dY);
    }
    alphas[R - 1] = fullAlpha - partialTriangleToAlpha(last, dY);
  }
}

void computeAlphaBelowLine(AlphaU8* alphas, FDot16 l, FDot16 r, FDot16 dY, AlphaU8 fullAlpha) {
  assert(l <= r);
  assert(l >> 16 == 0);
  int R = fdot16::ceilToI32(r);
  if (R == 0) {
    return;
  }
  if (R == 1) {
    alphas[0] = getPartialAlphaU8(trapezoidToAlpha(l, r), fullAlpha);
  } else {
    FDot16 first = fdot16::one - l;
    FDot16 last = r - ((R - 1) << 16);
    FDot16 lastH = fdot16::mul(last, dY);
    alphas[R - 1] = static_cast<AlphaU8>(fdot16::mul(last, lastH) >> 9);
    FDot16 alpha16 = sat_add(lastH, dY >> 1);
    for (int i = R - 2; i > 0; --i) {
      alphas[i] = static_cast<AlphaU8>((alpha16 >> 8) & 0xFF);
      alpha16 = sat_add(alpha16, dY);
    }
    alphas[0] = fullAlpha - partialTriangleToAlpha(first, dY);
  }
}

void blitSingleAlpha(AdditiveBlitter& blitter, int y, int x, AlphaU8 alpha, AlphaU8 fullAlpha) {
  if (fullAlpha == 0xFF) {
    blitter.getRealBlitter().blitV(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), 1,
                                   alpha);
  } else {
    blitter.blitAntiH(x, y, getPartialAlphaU8(alpha, fullAlpha));
  }
}

void blitTwoAlphas(AdditiveBlitter& blitter, int y, int x, AlphaU8 a1, AlphaU8 a2,
                   AlphaU8 fullAlpha) {
  if (fullAlpha == 0xFF) {
    blitter.getRealBlitter().blitAntiH2(static_cast<std::uint32_t>(x),
                                        static_cast<std::uint32_t>(y), a1, a2);
  } else {
    blitter.blitAntiH(x, y, a1);
    blitter.blitAntiH(x + 1, y, a2);
  }
}

void blitFullAlpha(AdditiveBlitter& blitter, int y, int x, int len, AlphaU8 fullAlpha,
                   bool noRealBlitter) {
  if (fullAlpha == 0xFF && !noRealBlitter) {
    blitter.getRealBlitter().blitH(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                   static_cast<LengthU32>(len));
  } else {
    blitter.blitAntiH(x, y, len, fullAlpha);
  }
}

void blitAaaTrapezoidRow(AdditiveBlitter& blitter, int y, FDot16 ul, FDot16 ur, FDot16 ll,
                         FDot16 lr, FDot16 lDY, FDot16 rDY, AlphaU8 fullAlpha,
                         bool noRealBlitter) {
  int L = fdot16::floorToI32(ul);
  int R = fdot16::ceilToI32(lr);
  int len = R - L;

  if (len == 1) {
    AlphaU8 alpha = trapezoidToAlpha(ur - ul, lr - ll);
    blitSingleAlpha(blitter, y, L, alpha, fullAlpha);
    return;
  }

  constexpr int kQuickLen = 513;
  AlphaU8 quickAlphas[(kQuickLen + 1) * 2];
  AlphaU8* alphas;
  AlphaU8* tempAlphas;

  if (len <= kQuickLen) {
    alphas = quickAlphas;
  } else {
    alphas = new AlphaU8[static_cast<std::size_t>((len + 1) * 2)];
  }
  tempAlphas = alphas + len + 1;

  for (int i = 0; i < len; ++i) {
    alphas[i] = fullAlpha;
  }

  int uL = fdot16::floorToI32(ul);
  int lL = fdot16::ceilToI32(ll);
  if (uL + 2 == lL) {
    FDot16 first = intToFixed(uL) + fdot16::one - ul;
    FDot16 second = ll - ul - first;
    AlphaU8 a1 = fullAlpha - partialTriangleToAlpha(first, lDY);
    AlphaU8 a2 = partialTriangleToAlpha(second, lDY);
    alphas[0] = alphas[0] > a1 ? alphas[0] - a1 : 0;
    alphas[1] = alphas[1] > a2 ? alphas[1] - a2 : 0;
  } else {
    std::memset(tempAlphas, 0, static_cast<std::size_t>(len + 1));
    computeAlphaBelowLine(tempAlphas + uL - L, ul - intToFixed(uL), ll - intToFixed(uL), lDY,
                          fullAlpha);
    for (int i = uL; i < lL; ++i) {
      if (alphas[i - L] > tempAlphas[i - L]) {
        alphas[i - L] -= tempAlphas[i - L];
      } else {
        alphas[i - L] = 0;
      }
    }
  }

  int uR = fdot16::floorToI32(ur);
  int lR = fdot16::ceilToI32(lr);
  if (uR + 2 == lR) {
    FDot16 first = intToFixed(uR) + fdot16::one - ur;
    FDot16 second = lr - ur - first;
    AlphaU8 a1 = partialTriangleToAlpha(first, rDY);
    AlphaU8 a2 = fullAlpha - partialTriangleToAlpha(second, rDY);
    alphas[len - 2] = alphas[len - 2] > a1 ? alphas[len - 2] - a1 : 0;
    alphas[len - 1] = alphas[len - 1] > a2 ? alphas[len - 1] - a2 : 0;
  } else {
    std::memset(tempAlphas, 0, static_cast<std::size_t>(len + 1));
    computeAlphaAboveLine(tempAlphas + uR - L, ur - intToFixed(uR), lr - intToFixed(uR), rDY,
                          fullAlpha);
    for (int i = uR; i < lR; ++i) {
      if (alphas[i - L] > tempAlphas[i - L]) {
        alphas[i - L] -= tempAlphas[i - L];
      } else {
        alphas[i - L] = 0;
      }
    }
  }

  if (fullAlpha == 0xFF && !noRealBlitter) {
    // Direct blit to real blitter, avoiding additive accumulation.
    // Instead of building per-pixel runs (3 vector allocations), decompose into:
    //   left partial pixels | full-coverage interior | right partial pixels
    auto& rb = blitter.getRealBlitter();
    auto uy = static_cast<std::uint32_t>(y);
    int i = 0;
    // Left partial pixels.
    while (i < len && alphas[i] != 0xFF) {
      if (alphas[i] > 0) {
        rb.blitAntiH2(static_cast<std::uint32_t>(L + i), uy, alphas[i], 0);
      }
      ++i;
    }
    // Full-coverage interior.
    int interiorStart = i;
    while (i < len && alphas[i] == 0xFF) {
      ++i;
    }
    if (i > interiorStart) {
      rb.blitH(static_cast<std::uint32_t>(L + interiorStart), uy,
               static_cast<LengthU32>(i - interiorStart));
    }
    // Right partial pixels.
    while (i < len) {
      if (alphas[i] > 0) {
        rb.blitAntiH2(static_cast<std::uint32_t>(L + i), uy, alphas[i], 0);
      }
      ++i;
    }
  } else {
    blitter.blitAntiHArray(L, y, alphas, len);
  }

  if (len > kQuickLen) {
    delete[] alphas;
  }
}

void blitTrapezoidRow(AdditiveBlitter& blitter, int y, FDot16 ul, FDot16 ur, FDot16 ll, FDot16 lr,
                      FDot16 lDY, FDot16 rDY, AlphaU8 fullAlpha, bool noRealBlitter) {
  assert(lDY >= 0 && rDY >= 0);

  if (ul > ur) return;

  if (ll > lr) {
    ll = lr = approximateIntersection(ul, ll, ur, lr);
  }

  if (ul == ur && ll == lr) return;

  if (ul > ll) std::swap(ul, ll);
  if (ur > lr) std::swap(ur, lr);

  FDot16 joinLeft = fixedCeilToFixed(ll);
  FDot16 joinRite = fixedFloorToFixed(ur);
  if (joinLeft <= joinRite) {
    if (ul < joinLeft) {
      int len = fdot16::ceilToI32(joinLeft - ul);
      if (len == 1) {
        AlphaU8 alpha = trapezoidToAlpha(joinLeft - ul, joinLeft - ll);
        blitSingleAlpha(blitter, y, ul >> 16, alpha, fullAlpha);
      } else if (len == 2) {
        FDot16 first = joinLeft - fdot16::one - ul;
        FDot16 second = ll - ul - first;
        AlphaU8 a1 = partialTriangleToAlpha(first, lDY);
        AlphaU8 a2 = fullAlpha - partialTriangleToAlpha(second, lDY);
        blitTwoAlphas(blitter, y, ul >> 16, a1, a2, fullAlpha);
      } else {
        blitAaaTrapezoidRow(blitter, y, ul, joinLeft, ll, joinLeft, lDY,
                            std::numeric_limits<FDot16>::max(), fullAlpha, noRealBlitter);
      }
    }
    if (joinLeft < joinRite) {
      blitFullAlpha(blitter, y, fdot16::floorToI32(joinLeft),
                    fdot16::floorToI32(joinRite - joinLeft), fullAlpha, noRealBlitter);
    }
    if (lr > joinRite) {
      int len = fdot16::ceilToI32(lr - joinRite);
      if (len == 1) {
        AlphaU8 alpha = trapezoidToAlpha(ur - joinRite, lr - joinRite);
        blitSingleAlpha(blitter, y, joinRite >> 16, alpha, fullAlpha);
      } else if (len == 2) {
        FDot16 first = joinRite + fdot16::one - ur;
        FDot16 second = lr - ur - first;
        AlphaU8 a1 = fullAlpha - partialTriangleToAlpha(first, rDY);
        AlphaU8 a2 = partialTriangleToAlpha(second, rDY);
        blitTwoAlphas(blitter, y, joinRite >> 16, a1, a2, fullAlpha);
      } else {
        blitAaaTrapezoidRow(blitter, y, joinRite, ur, joinRite, lr,
                            std::numeric_limits<FDot16>::max(), rDY, fullAlpha, noRealBlitter);
      }
    }
  } else {
    blitAaaTrapezoidRow(blitter, y, ul, ur, ll, lr, lDY, rDY, fullAlpha, noRealBlitter);
  }
}

// ── Edge linked list operations ──────────────────────────────────────────────

void removeEdge(AnalyticEdge* edge) {
  edge->prev->next = edge->next;
  edge->next->prev = edge->prev;
}

void insertEdgeAfter(AnalyticEdge* edge, AnalyticEdge* after) {
  edge->prev = after;
  edge->next = after->next;
  after->next->prev = edge;
  after->next = edge;
}

AnalyticEdge* backwardInsertStart(AnalyticEdge* prev, FDot16 x) {
  while (prev->prev && prev->x > x) {
    prev = prev->prev;
  }
  return prev;
}

void backwardInsertEdgeBasedOnX(AnalyticEdge* edge) {
  auto* prev = edge->prev;
  if (prev->prev == nullptr || prev->x <= edge->x) {
    return;
  }
  auto* start = backwardInsertStart(prev, edge->x);
  if (start->next != edge) {
    removeEdge(edge);
    insertEdgeAfter(edge, start);
  }
}

void updateNextNextY(FDot16 y, FDot16 nextY, FDot16* nextNextY) {
  if (y > nextY && y < *nextNextY) {
    *nextNextY = y;
  }
}

void checkIntersection(const AnalyticEdge* edge, FDot16 nextY, FDot16* nextNextY) {
  if (edge->prev->prev && edge->prev->x + edge->prev->dx > edge->x + edge->dx) {
    *nextNextY = nextY + (fdot16::one >> AnalyticEdge::kDefaultAccuracy);
  }
}

void checkIntersectionFwd(const AnalyticEdge* edge, FDot16 nextY, FDot16* nextNextY) {
  if (edge->next->next && edge->x + edge->dx > edge->next->x + edge->next->dx) {
    *nextNextY = nextY + (fdot16::one >> AnalyticEdge::kDefaultAccuracy);
  }
}

void insertNewEdges(AnalyticEdge* newEdge, FDot16 y, FDot16* nextNextY) {
  if (newEdge->upperY > y) {
    updateNextNextY(newEdge->upperY, y, nextNextY);
    return;
  }
  AnalyticEdge* prev = newEdge->prev;
  if (prev->x <= newEdge->x) {
    while (newEdge->upperY <= y) {
      checkIntersection(newEdge, y, nextNextY);
      updateNextNextY(newEdge->lowerY, y, nextNextY);
      newEdge = newEdge->next;
    }
    updateNextNextY(newEdge->upperY, y, nextNextY);
    return;
  }
  auto* start = backwardInsertStart(prev, newEdge->x);
  do {
    AnalyticEdge* next = newEdge->next;
    while (true) {
      if (start->next == newEdge) {
        goto nextEdge;
      }
      AnalyticEdge* after = start->next;
      if (after->x >= newEdge->x) {
        break;
      }
      assert(start != after);
      start = after;
    }
    removeEdge(newEdge);
    insertEdgeAfter(newEdge, start);
  nextEdge:
    checkIntersection(newEdge, y, nextNextY);
    checkIntersectionFwd(newEdge, y, nextNextY);
    updateNextNextY(newEdge->lowerY, y, nextNextY);
    start = newEdge;
    newEdge = next;
  } while (newEdge->upperY <= y);
  updateNextNextY(newEdge->upperY, y, nextNextY);
}

bool edgesTooClose(AnalyticEdge* prev, AnalyticEdge* next, FDot16 lowerY) {
  return next && prev && next->upperY < lowerY &&
         prev->x + fdot16::one >= next->x - std::abs(next->dx);
}

bool edgesTooClose(int prevRite, FDot16 ul, FDot16 ll) {
  return prevRite > fdot16::floorToI32(ul) || prevRite > fdot16::floorToI32(ll);
}

// ── Smooth-enough check for convex walker ────────────────────────────────────
// For an edge, we consider it smooth if Dx doesn't change much and Dy is large enough.
// This matches Skia's is_smooth_enough exactly.

FDot16 sat_sub(FDot16 a, FDot16 b) {
  auto r = static_cast<std::int64_t>(a) - b;
  if (r > std::numeric_limits<std::int32_t>::max()) {
    return std::numeric_limits<std::int32_t>::max();
  }
  if (r < std::numeric_limits<std::int32_t>::min()) {
    return std::numeric_limits<std::int32_t>::min();
  }
  return static_cast<std::int32_t>(r);
}

bool isSmoothEnough(AnalyticEdge* thisEdge, AnalyticEdge* nextEdge, int stopY) {
  if (thisEdge->curveCount < 0) {
    // Cubic edge.
    int ddshift = thisEdge->curveShift;
    return std::abs(thisEdge->cdx) >> 1 >= std::abs(thisEdge->cddx) >> ddshift &&
           std::abs(thisEdge->cdy) >> 1 >= std::abs(thisEdge->cddy) >> ddshift &&
           (thisEdge->cdy - (thisEdge->cddy >> ddshift)) >> thisEdge->cubicDShift >= fdot16::one;
  } else if (thisEdge->curveCount > 0) {
    // Quadratic edge.
    return std::abs(thisEdge->qdx) >> 1 >= std::abs(thisEdge->qddx) &&
           std::abs(thisEdge->qdy) >> 1 >= std::abs(thisEdge->qddy) &&
           (thisEdge->qdy - thisEdge->qddy) >> thisEdge->curveShift >= fdot16::one;
  }
  // Line edge: DDx should be small and Dy should be large.
  return std::abs(sat_sub(nextEdge->dx, thisEdge->dx)) <= fdot16::one &&
         nextEdge->lowerY - nextEdge->upperY >= fdot16::one;
}

bool isSmoothEnough(AnalyticEdge* leftE, AnalyticEdge* riteE, AnalyticEdge* currE, int stopY) {
  if (currE->upperY >= intToFixed(stopY)) {
    return false;  // At the end, won't skip anything.
  }
  if (leftE->lowerY + fdot16::one < riteE->lowerY) {
    return isSmoothEnough(leftE, currE, stopY);  // Only leftE is changing.
  } else if (leftE->lowerY > riteE->lowerY + fdot16::one) {
    return isSmoothEnough(riteE, currE, stopY);  // Only riteE is changing.
  }

  // Both edges are changing; find the second next edge.
  AnalyticEdge* nextCurrE = currE->next;
  if (nextCurrE->upperY >= intToFixed(stopY)) {
    return false;
  }
  // Ensure currE is next left edge, nextCurrE is next right edge.
  if (nextCurrE->upperX < currE->upperX) {
    std::swap(currE, nextCurrE);
  }
  return isSmoothEnough(leftE, currE, stopY) && isSmoothEnough(riteE, nextCurrE, stopY);
}

// ── Convex edge walker ──────────────────────────────────────────────────────
// Optimized walker for convex paths: only tracks two edges (left/right).
// Matches Skia's aaa_walk_convex_edges for bit-exact coverage.

void aaaWalkConvexEdges(AnalyticEdge* prevHead, AdditiveBlitter& blitter, int startY, int stopY,
                        FDot16 leftBound, FDot16 riteBound) {
  AnalyticEdge* leftE = prevHead->next;
  AnalyticEdge* riteE = leftE->next;
  AnalyticEdge* currE = riteE->next;

  FDot16 y = std::max(leftE->upperY, riteE->upperY);

  for (;;) {
    // Advance left edge past expired segments.
    while (leftE->lowerY <= y) {
      if (!leftE->update(y)) {
        if (fdot16::floorToI32(currE->upperY) >= stopY) {
          goto END_WALK;
        }
        leftE = currE;
        currE = currE->next;
      }
    }
    // Advance right edge past expired segments.
    while (riteE->lowerY <= y) {
      if (!riteE->update(y)) {
        if (fdot16::floorToI32(currE->upperY) >= stopY) {
          goto END_WALK;
        }
        riteE = currE;
        currE = currE->next;
      }
    }

    if (fdot16::floorToI32(y) >= stopY) {
      break;
    }

    leftE->goY(y);
    riteE->goY(y);

    if (leftE->x > riteE->x || (leftE->x == riteE->x && leftE->dx > riteE->dx)) {
      std::swap(leftE, riteE);
    }

    FDot16 localBotFixed = std::min(leftE->lowerY, riteE->lowerY);
    if (isSmoothEnough(leftE, riteE, currE, stopY)) {
      localBotFixed = fixedCeilToFixed(localBotFixed);
    }
    localBotFixed = std::min(localBotFixed, intToFixed(stopY));

    FDot16 left = std::max(leftBound, leftE->x);
    FDot16 dLeft = leftE->dx;
    FDot16 rite = std::min(riteBound, riteE->x);
    FDot16 dRite = riteE->dx;

    if (0 == (dLeft | dRite)) {
      // Zero-slope case: axis-aligned rect optimization.
      int fullLeft = fdot16::ceilToI32(left);
      int fullRite = fdot16::floorToI32(rite);
      FDot16 partialLeft = intToFixed(fullLeft) - left;
      FDot16 partialRite = rite - intToFixed(fullRite);
      int fullTop = fdot16::ceilToI32(y);
      int fullBot = fdot16::floorToI32(localBotFixed);
      FDot16 partialTop = intToFixed(fullTop) - y;
      FDot16 partialBot = localBotFixed - intToFixed(fullBot);
      if (fullTop > fullBot) {
        partialTop -= (fdot16::one - partialBot);
        partialBot = 0;
      }

      if (fullRite >= fullLeft) {
        if (partialTop > 0) {
          if (partialLeft > 0) {
            blitter.blitAntiH(fullLeft - 1, fullTop - 1,
                              fixedToAlpha(fdot16::mul(partialTop, partialLeft)));
          }
          blitter.blitAntiH(fullLeft, fullTop - 1, fullRite - fullLeft,
                            fixedToAlpha(partialTop));
          if (partialRite > 0) {
            blitter.blitAntiH(fullRite, fullTop - 1,
                              fixedToAlpha(fdot16::mul(partialTop, partialRite)));
          }
          blitter.flushIfYChanged(y, y + partialTop);
        }

        if (fullBot > fullTop &&
            (fullRite > fullLeft || fixedToAlpha(partialLeft) > 0 ||
             fixedToAlpha(partialRite) > 0)) {
          blitter.getRealBlitter().blitAntiRect(fullLeft - 1, fullTop, fullRite - fullLeft,
                                                fullBot - fullTop, fixedToAlpha(partialLeft),
                                                fixedToAlpha(partialRite));
        }

        if (partialBot > 0) {
          if (partialLeft > 0) {
            blitter.blitAntiH(fullLeft - 1, fullBot,
                              fixedToAlpha(fdot16::mul(partialBot, partialLeft)));
          }
          blitter.blitAntiH(fullLeft, fullBot, fullRite - fullLeft, fixedToAlpha(partialBot));
          if (partialRite > 0) {
            blitter.blitAntiH(fullRite, fullBot,
                              fixedToAlpha(fdot16::mul(partialBot, partialRite)));
          }
        }
      } else {
        FDot16 width = rite - left;
        if (width > 0) {
          if (partialTop > 0) {
            blitter.blitAntiH(fullLeft - 1, fullTop - 1, 1,
                              fixedToAlpha(fdot16::mul(partialTop, width)));
            blitter.flushIfYChanged(y, y + partialTop);
          }
          if (fullBot > fullTop) {
            blitter.getRealBlitter().blitV(static_cast<std::uint32_t>(fullLeft - 1),
                                           static_cast<std::uint32_t>(fullTop),
                                           static_cast<LengthU32>(fullBot - fullTop),
                                           fixedToAlpha(width));
          }
          if (partialBot > 0) {
            blitter.blitAntiH(fullLeft - 1, fullBot, 1,
                              fixedToAlpha(fdot16::mul(partialBot, width)));
          }
        }
      }

      y = localBotFixed;
    } else {
      // Non-zero slope: row-by-row trapezoid blitting with X snapping.
      constexpr FDot16 kSnapDigit = fdot16::one >> 4;
      constexpr FDot16 kSnapHalf = kSnapDigit >> 1;
      constexpr FDot16 kSnapMask = ~(kSnapDigit - 1);
      left += kSnapHalf;
      rite += kSnapHalf;

      int count = fdot16::ceilToI32(localBotFixed) - fdot16::floorToI32(y);

      if (count > 1) {
        if (static_cast<std::int32_t>(y & 0xFFFF0000) != y) {
          // Partial top row.
          count--;
          FDot16 nextY = fixedCeilToFixed(y + 1);
          FDot16 dY = nextY - y;
          FDot16 nextLeft = left + fdot16::mul(dLeft, dY);
          FDot16 nextRite = rite + fdot16::mul(dRite, dY);
          blitTrapezoidRow(blitter, y >> 16, left & kSnapMask, rite & kSnapMask,
                           nextLeft & kSnapMask, nextRite & kSnapMask, leftE->dy, riteE->dy,
                           getPartialAlpha(0xFF, dY), false);
          blitter.flushIfYChanged(y, nextY);
          left = nextLeft;
          rite = nextRite;
          y = nextY;
        }

        while (count > 1) {
          // Full rows in the middle.
          count--;
          FDot16 nextY = y + fdot16::one;
          FDot16 nextLeft = left + dLeft;
          FDot16 nextRite = rite + dRite;
          blitTrapezoidRow(blitter, y >> 16, left & kSnapMask, rite & kSnapMask,
                           nextLeft & kSnapMask, nextRite & kSnapMask, leftE->dy, riteE->dy,
                           0xFF, false);
          blitter.flushIfYChanged(y, nextY);
          left = nextLeft;
          rite = nextRite;
          y = nextY;
        }
      }

      // Partial bottom row.
      FDot16 dY = localBotFixed - y;
      FDot16 nextLeft = std::max(left + fdot16::mul(dLeft, dY), leftBound + kSnapHalf);
      FDot16 nextRite = std::min(rite + fdot16::mul(dRite, dY), riteBound + kSnapHalf);
      blitTrapezoidRow(blitter, y >> 16, left & kSnapMask, rite & kSnapMask,
                       nextLeft & kSnapMask, nextRite & kSnapMask, leftE->dy, riteE->dy,
                       getPartialAlpha(0xFF, dY), false);
      blitter.flushIfYChanged(y, localBotFixed);
      left = nextLeft;
      rite = nextRite;
      y = localBotFixed;
      left -= kSnapHalf;
      rite -= kSnapHalf;
    }

    leftE->x = left;
    riteE->x = rite;
    leftE->y = riteE->y = y;
  }

END_WALK:;
}

// ── Edge comparison and sorting ──────────────────────────────────────────────

bool compareEdges(const AnalyticEdge* a, const AnalyticEdge* b) {
  if (a->upperY != b->upperY) return a->upperY < b->upperY;
  if (a->x != b->x) return a->x < b->x;
  return a->dx < b->dx;
}

// ── Main analytic edge walker ────────────────────────────────────────────────

void aaaWalkEdges(AnalyticEdge* prevHead, AnalyticEdge* nextTail, FillRule fillRule,
                  AdditiveBlitter& blitter, int startY, int stopY, FDot16 leftClip,
                  FDot16 rightClip, bool skipIntersect) {
  prevHead->x = prevHead->upperX = leftClip;
  nextTail->x = nextTail->upperX = rightClip;
  FDot16 y = std::max(prevHead->next->upperY, intToFixed(startY));
  FDot16 nextNextY = std::numeric_limits<FDot16>::max();

  {
    AnalyticEdge* edge;
    for (edge = prevHead->next; edge->upperY <= y; edge = edge->next) {
      edge->goY(y);
      updateNextNextY(edge->lowerY, y, &nextNextY);
    }
    updateNextNextY(edge->upperY, y, &nextNextY);
  }

  int windingMask = fillRule == FillRule::EvenOdd ? 1 : -1;

  while (true) {
    int w = 0;
    bool inInterval = false;
    FDot16 prevX = prevHead->x;
    FDot16 nextY = std::min(nextNextY, fixedCeilToFixed(y + 1));
    AnalyticEdge* currE = prevHead->next;
    AnalyticEdge* leftE = prevHead;
    FDot16 left = leftClip;
    FDot16 leftDY = 0;
    int prevRite = fdot16::floorToI32(leftClip);

    nextNextY = std::numeric_limits<FDot16>::max();

    int yShift = 0;
    if ((nextY - y) & (fdot16::one >> 2)) {
      yShift = 2;
      nextY = y + (fdot16::one >> 2);
    } else if ((nextY - y) & (fdot16::one >> 1)) {
      yShift = 1;
    }

    AlphaU8 fullAlpha = fixedToAlpha(nextY - y);

    bool noRealBlitter = false;

    while (currE->upperY <= y) {
      assert(currE->lowerY >= nextY);

      w += static_cast<int>(currE->winding);
      bool prevInInterval = inInterval;
      inInterval = (w & windingMask) != 0;

      bool isLeft = inInterval && !prevInInterval;
      bool isRite = !inInterval && prevInInterval;

      if (isRite) {
        FDot16 rite = currE->x;
        currE->goY(nextY, yShift);
        FDot16 nextLeft = std::max(leftClip, leftE->x);
        rite = std::min(rightClip, rite);
        FDot16 nextRite = std::min(rightClip, currE->x);
        blitTrapezoidRow(blitter, y >> 16, left, rite, nextLeft, nextRite, leftDY, currE->dy,
                         fullAlpha,
                         noRealBlitter ||
                             (fullAlpha == 0xFF && (edgesTooClose(prevRite, left, leftE->x) ||
                                                    edgesTooClose(currE, currE->next, nextY))));
        prevRite = fdot16::ceilToI32(std::max(rite, currE->x));
      } else {
        if (isLeft) {
          left = std::max(currE->x, leftClip);
          leftDY = currE->dy;
          leftE = currE;
        }
        currE->goY(nextY, yShift);
      }

      AnalyticEdge* next = currE->next;
      FDot16 newX;

      while (currE->lowerY <= nextY) {
        if (currE->curveCount < 0) {
          currE->keepContinuousCubic();
          if (!currE->updateCubic()) break;
        } else if (currE->curveCount > 0) {
          currE->keepContinuousQuad();
          if (!currE->updateQuadratic()) break;
        } else {
          break;
        }
      }

      if (currE->lowerY <= nextY) {
        removeEdge(currE);
      } else {
        updateNextNextY(currE->lowerY, nextY, &nextNextY);
        newX = currE->x;
        if (newX < prevX) {
          backwardInsertEdgeBasedOnX(currE);
        } else {
          prevX = newX;
        }
        if (!skipIntersect) {
          checkIntersection(currE, nextY, &nextNextY);
        }
      }

      currE = next;
    }

    if (inInterval) {
      blitTrapezoidRow(blitter, y >> 16, left, rightClip, std::max(leftClip, leftE->x), rightClip,
                       leftDY, 0, fullAlpha,
                       noRealBlitter ||
                           (fullAlpha == 0xFF && edgesTooClose(leftE->prev, leftE, nextY)));
    }

    y = nextY;
    if (y >= intToFixed(stopY)) break;

    insertNewEdges(currE, y, &nextNextY);
  }
}

// ── Analytic edge builder ────────────────────────────────────────────────────

bool isFinitePoint(Point p) { return std::isfinite(p.x) && std::isfinite(p.y); }

bool isNotMonotonic(float a, float b, float c) {
  float ab = a - b;
  float bc = b - c;
  if (ab < 0.0f) bc = -bc;
  return ab == 0.0f || bc < 0.0f;
}

void chopQuadAt(const std::array<Point, 3>& src, float t, std::array<Point, 5>& dst) {
  auto interp = [](float v0, float v1, float tt) { return v0 + (v1 - v0) * tt; };
  Point p01{interp(src[0].x, src[1].x, t), interp(src[0].y, src[1].y, t)};
  Point p12{interp(src[1].x, src[2].x, t), interp(src[1].y, src[2].y, t)};
  Point p012{interp(p01.x, p12.x, t), interp(p01.y, p12.y, t)};
  dst = {src[0], p01, p012, p12, src[2]};
}

std::size_t chopQuadAtYExtrema(const std::array<Point, 3>& src, std::array<Point, 5>& dst) {
  float a = src[0].y, b = src[1].y, c = src[2].y;
  if (isNotMonotonic(a, b, c)) {
    float numer = a - b, denom = a - b - b + c;
    if (numer < 0.0f) {
      numer = -numer;
      denom = -denom;
    }
    if (denom != 0.0f && numer != 0.0f && numer < denom) {
      float t = numer / denom;
      if (t > 0.0f && t < 1.0f) {
        chopQuadAt(src, t, dst);
        dst[1].y = dst[2].y;
        dst[3].y = dst[2].y;
        return 1;
      }
    }
    b = std::abs(a - b) < std::abs(b - c) ? a : c;
  }
  dst[0] = {src[0].x, a};
  dst[1] = {src[1].x, b};
  dst[2] = {src[2].x, c};
  return 0;
}

void chopCubicAt2(std::array<Point, 4> src, float t, std::array<Point, 7>& dst) {
  auto interp = [](float v0, float v1, float tt) { return v0 + (v1 - v0) * tt; };
  float abx = interp(src[0].x, src[1].x, t), aby = interp(src[0].y, src[1].y, t);
  float bcx = interp(src[1].x, src[2].x, t), bcy = interp(src[1].y, src[2].y, t);
  float cdx = interp(src[2].x, src[3].x, t), cdy = interp(src[2].y, src[3].y, t);
  float abcx = interp(abx, bcx, t), abcy = interp(aby, bcy, t);
  float bcdx = interp(bcx, cdx, t), bcdy = interp(bcy, cdy, t);
  float abcdx = interp(abcx, bcdx, t), abcdy = interp(abcy, bcdy, t);
  dst = {src[0], {abx, aby}, {abcx, abcy}, {abcdx, abcdy}, {bcdx, bcdy}, {cdx, cdy}, src[3]};
}

std::size_t chopCubicAtYExtrema(const std::array<Point, 4>& src, std::array<Point, 10>& dst) {
  auto tValuesF = pathGeometry::newTValues();
  auto rawCount =
      pathGeometry::findCubicExtremaT(src[0].y, src[1].y, src[2].y, src[3].y, tValuesF.data());

  if (rawCount == 0) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
    return 0;
  }

  std::array<float, 3> tValues{};
  for (std::size_t i = 0; i < rawCount; ++i) {
    tValues[i] = tValuesF[i].get();
  }

  // Chop at first t value.
  std::array<Point, 7> split{};
  chopCubicAt2(src, tValues[0], split);
  dst[0] = split[0];
  dst[1] = split[1];
  dst[2] = split[2];
  dst[3] = split[3];

  if (rawCount == 1) {
    dst[4] = split[4];
    dst[5] = split[5];
    dst[6] = split[6];
  } else {
    std::array<Point, 4> remaining = {split[3], split[4], split[5], split[6]};
    float diff = tValues[1] - tValues[0];
    float base = 1.0f - tValues[0];
    float t2 = (base != 0.0f && diff > 0.0f && diff < base) ? diff / base : 0.0f;
    if (t2 > 0.0f && t2 < 1.0f) {
      std::array<Point, 7> split2{};
      chopCubicAt2(remaining, t2, split2);
      dst[3] = split2[0];
      dst[4] = split2[1];
      dst[5] = split2[2];
      dst[6] = split2[3];
      dst[7] = split2[4];
      dst[8] = split2[5];
      dst[9] = split2[6];
    } else {
      dst[4] = remaining[0];
      dst[5] = remaining[1];
      dst[6] = remaining[2];
      dst[7] = remaining[3];
      dst[8] = remaining[3];
      dst[9] = remaining[3];
    }
  }

  // Fix Y monotonicity at split points.
  if (rawCount > 0) {
    dst[2].y = dst[3].y;
    dst[4].y = dst[3].y;
    if (rawCount == 2) {
      dst[5].y = dst[6].y;
      dst[7].y = dst[6].y;
    }
  }

  return rawCount;
}

std::vector<AnalyticEdge> buildAnalyticEdges(const Path& path, const ScreenIntRect* clipRect,
                                             bool pathContainedInClip) {
  std::vector<AnalyticEdge> edges;

  auto pushLine = [&](Point p0, Point p1) {
    AnalyticEdge edge;
    if (edge.setLine(p0, p1)) {
      edges.push_back(edge);
    }
  };

  auto pushQuad = [&](const Point pts[3]) {
    AnalyticEdge edge;
    if (edge.setQuadratic(pts)) {
      edges.push_back(edge);
    }
  };

  auto pushCubic = [&](const Point pts[4]) {
    AnalyticEdge edge;
    if (edge.setCubic(pts)) {
      edges.push_back(edge);
    }
  };

  if (clipRect != nullptr && !pathContainedInClip) {
    Rect clipRectF = clipRect->toRect();
    for (auto iterator = EdgeClipperIter(path, clipRectF, false);;) {
      auto clippedEdges = iterator.next();
      if (!clippedEdges.has_value()) break;

      for (const auto& edgeValue : clippedEdges->span()) {
        if (!isFinitePoint(edgeValue.points[0]) || !isFinitePoint(edgeValue.points[1])) continue;
        switch (edgeValue.type) {
          case PathEdgeType::LineTo:
            pushLine(edgeValue.points[0], edgeValue.points[1]);
            break;
          case PathEdgeType::QuadTo:
            if (!isFinitePoint(edgeValue.points[2])) continue;
            pushQuad(&edgeValue.points[0]);
            break;
          case PathEdgeType::CubicTo:
            if (!isFinitePoint(edgeValue.points[2]) || !isFinitePoint(edgeValue.points[3]))
              continue;
            pushCubic(&edgeValue.points[0]);
            break;
        }
      }
    }
  } else {
    for (auto iterator = pathIter(path);;) {
      auto edge = iterator.next();
      if (!edge.has_value()) break;

      const auto& edgeValue = edge.value();
      if (!isFinitePoint(edgeValue.points[0]) || !isFinitePoint(edgeValue.points[1])) continue;

      switch (edgeValue.type) {
        case PathEdgeType::LineTo:
          pushLine(edgeValue.points[0], edgeValue.points[1]);
          break;
        case PathEdgeType::QuadTo: {
          if (!isFinitePoint(edgeValue.points[2])) continue;
          std::array<Point, 3> pts = {edgeValue.points[0], edgeValue.points[1],
                                      edgeValue.points[2]};
          std::array<Point, 5> mono{};
          auto count = chopQuadAtYExtrema(pts, mono);
          for (std::size_t i = 0; i <= count; ++i) {
            pushQuad(&mono[i * 2]);
          }
          break;
        }
        case PathEdgeType::CubicTo: {
          if (!isFinitePoint(edgeValue.points[2]) || !isFinitePoint(edgeValue.points[3])) continue;
          std::array<Point, 4> pts = {edgeValue.points[0], edgeValue.points[1],
                                      edgeValue.points[2], edgeValue.points[3]};
          std::array<Point, 10> mono{};
          auto count = chopCubicAtYExtrema(pts, mono);
          for (std::size_t i = 0; i <= count; ++i) {
            pushCubic(&mono[i * 3]);
          }
          break;
        }
      }
    }
  }

  return edges;
}

// ── Main fill function ───────────────────────────────────────────────────────

void aaaFillPath(const Path& path, FillRule fillRule, const ScreenIntRect& clipRect,
                 AdditiveBlitter& blitter, int startY, int stopY, bool pathContainedInClip) {
  auto edges = buildAnalyticEdges(path, &clipRect, pathContainedInClip);

  if (edges.size() < 2) return;

  // Sort edge pointers by upperY, then x, then dx.
  std::vector<AnalyticEdge*> edgePtrs;
  edgePtrs.reserve(edges.size());
  for (auto& e : edges) {
    edgePtrs.push_back(&e);
  }
  std::sort(edgePtrs.begin(), edgePtrs.end(), compareEdges);

  // Link into doubly-linked list.
  for (std::size_t i = 1; i < edgePtrs.size(); ++i) {
    edgePtrs[i - 1]->next = edgePtrs[i];
    edgePtrs[i]->prev = edgePtrs[i - 1];
  }

  AnalyticEdge headEdge, tailEdge;

  headEdge.prev = nullptr;
  headEdge.next = edgePtrs.front();
  headEdge.upperY = headEdge.lowerY = std::numeric_limits<FDot16>::min();
  headEdge.x = std::numeric_limits<FDot16>::min();
  headEdge.dx = 0;
  headEdge.dy = std::numeric_limits<FDot16>::max();
  headEdge.upperX = std::numeric_limits<FDot16>::min();
  edgePtrs.front()->prev = &headEdge;

  tailEdge.prev = edgePtrs.back();
  tailEdge.next = nullptr;
  tailEdge.upperY = tailEdge.lowerY = std::numeric_limits<FDot16>::max();
  tailEdge.x = std::numeric_limits<FDot16>::max();
  tailEdge.dx = 0;
  tailEdge.dy = std::numeric_limits<FDot16>::max();
  tailEdge.upperX = std::numeric_limits<FDot16>::max();
  edgePtrs.back()->next = &tailEdge;

  if (!pathContainedInClip && startY < static_cast<int>(clipRect.y())) {
    startY = static_cast<int>(clipRect.y());
  }
  if (!pathContainedInClip && stopY > static_cast<int>(clipRect.bottom())) {
    stopY = static_cast<int>(clipRect.bottom());
  }

  FDot16 leftBound = intToFixed(static_cast<std::int32_t>(clipRect.x()));
  FDot16 rightBound = intToFixed(static_cast<std::int32_t>(clipRect.right()));

  if (path.isConvex() && edges.size() >= 2) {
    aaaWalkConvexEdges(&headEdge, blitter, startY, stopY, leftBound, rightBound);
  } else {
    bool skipIntersect = static_cast<int>(path.points().size()) > (stopY - startY) * 2;
    aaaWalkEdges(&headEdge, &tailEdge, fillRule, blitter, startY, stopY, leftBound, rightBound,
                 skipIntersect);
  }
}

}  // namespace

namespace scan::path_aa {

void fillPath(const Path& path, FillRule fillRule, const ScreenIntRect& clip, Blitter& blitter) {
  const auto boundsOpt = path.bounds().roundOut();
  if (!boundsOpt) return;

  const auto clipped = boundsOpt->intersect(clip.toIntRect());
  if (!clipped) return;

  if (clip.right() > 32767 || clip.bottom() > 32767) return;

  const auto pathContainedInClip = [&]() {
    const auto boundsScreen = boundsOpt->toScreenIntRect();
    return boundsScreen.has_value() && clip.contains(boundsScreen.value());
  }();

  AdditiveBlitter additiveBlitter(blitter, static_cast<std::int32_t>(clip.x()),
                                  static_cast<std::int32_t>(clip.width()),
                                  static_cast<std::int32_t>(clip.y()),
                                  static_cast<std::int32_t>(clip.height()));

  aaaFillPath(path, fillRule, clip, additiveBlitter, static_cast<int>(boundsOpt->y()),
              static_cast<int>(boundsOpt->y() + boundsOpt->height()), pathContainedInClip);
}

void fillPathImpl(const Path& path, FillRule fillRule, const IntRect& bounds,
                  const ScreenIntRect& clipRect, std::int32_t startY, std::int32_t stopY,
                  std::int32_t /*shiftEdgesUp*/, bool pathContainedInClip, Blitter& blitter) {
  // With AAA, we ignore shiftEdgesUp since we don't super-sample.
  AdditiveBlitter additiveBlitter(blitter, static_cast<std::int32_t>(clipRect.x()),
                                  static_cast<std::int32_t>(clipRect.width()),
                                  static_cast<std::int32_t>(clipRect.y()),
                                  static_cast<std::int32_t>(clipRect.height()));

  aaaFillPath(path, fillRule, clipRect, additiveBlitter, startY, stopY, pathContainedInClip);
}

}  // namespace scan::path_aa

}  // namespace tiny_skia
