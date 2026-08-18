#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "tiny_skia/AlphaRuns.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Mask.h"

namespace tiny_skia {

class Blitter;
struct Paint;

class Blitter {
 public:
  Blitter() = default;
  virtual ~Blitter() = default;

  virtual void blitH(std::uint32_t x, std::uint32_t y, LengthU32 width);
  virtual void blitAntiH(std::uint32_t x, std::uint32_t y, std::span<std::uint8_t> alpha,
                         std::span<AlphaRun> runs);
  virtual void blitV(std::uint32_t x, std::uint32_t y, LengthU32 height, AlphaU8 alpha);
  virtual void blitAntiH2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1);
  virtual void blitAntiV2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1);
  virtual void blitAntiRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                            AlphaU8 leftAlpha, AlphaU8 rightAlpha);
  virtual void blitRect(const ScreenIntRect& rect);
  virtual void blitMask(const Mask& mask, const ScreenIntRect& clip);

 private:
  void unreachable() const;
};

/// Interception point for the blitter a draw hands to its scan converter.
///
/// A draw builds one pipeline blitter per scan pass and drives the scan converter with it.
/// Passing a wrapper to that draw lets a caller substitute a blitter that forwards to the
/// pipeline blitter, which is how the blit sequence can be observed without changing what the
/// draw paints. Returning `pipelineBlitter` unchanged is always valid.
class BlitterWrapper {
 public:
  BlitterWrapper() = default;
  virtual ~BlitterWrapper() = default;

  BlitterWrapper(const BlitterWrapper&) = delete;
  BlitterWrapper& operator=(const BlitterWrapper&) = delete;

  /// Returns the blitter the scan converter should drive.
  ///
  /// @param pipelineBlitter Blitter the draw built. It outlives the scan pass, so the
  ///   returned blitter may hold a reference to it.
  /// @param paint Paint `pipelineBlitter` was built from. A draw may adjust the caller's
  ///   paint before building the blitter (hairline strokes fold their coverage into the
  ///   shader opacity, and a transformed draw transforms the shader), so this is the paint a
  ///   replay must reuse to reproduce the draw exactly.
  virtual Blitter& wrap(Blitter& pipelineBlitter, const Paint& paint) = 0;
};

}  // namespace tiny_skia
