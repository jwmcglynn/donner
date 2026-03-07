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

}  // namespace tiny_skia
