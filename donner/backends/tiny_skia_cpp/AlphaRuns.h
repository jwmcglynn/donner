#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace donner::backends::tiny_skia_cpp {

/** Sparse run-length-encoded coverage buffer used during rasterization. */
class AlphaRuns {
public:
  /// Constructs a run buffer sized to a scanline width.
  explicit AlphaRuns(uint32_t width);

  /** Returns true when the scanline contains only transparent coverage. */
  bool isEmpty() const;

  /** Reinitializes the run buffer for a new scanline. */
  void reset(uint32_t width);

  /**
   * Inserts a coverage run starting at \a x adjusted by \a offsetX.
   *
   * The run contributes a starting pixel, optional middle region, and ending pixel using
   * per-pixel alpha values. Returns the offset that should be reused for subsequent runs on the
   * same scanline to avoid rescanning the prefix. Callers should only reuse the returned offset
   * when the next span's x coordinate is greater than or equal to the prior offset.
   */
  size_t add(uint32_t x, uint8_t startAlpha, size_t middleCount, uint8_t stopAlpha,
             uint8_t maxValue, size_t offsetX);

  /// Returns 0-255 given 0-256 coverage.
  static uint8_t catchOverflow(uint16_t alpha);

  /// Accessor for encoded run lengths.
  const std::vector<uint16_t>& runs() const { return runs_; }

  /// Accessor for per-run alpha values.
  const std::vector<uint8_t>& alpha() const { return alpha_; }

private:
  static void breakRun(std::span<uint16_t> runs, std::span<uint8_t> alpha, size_t x, size_t count);

  std::vector<uint16_t> runs_;
  std::vector<uint8_t> alpha_;
};

}  // namespace donner::backends::tiny_skia_cpp
