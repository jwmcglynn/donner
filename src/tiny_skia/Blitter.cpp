#include "tiny_skia/Blitter.h"

#include "tiny_skia/Geom.h"

namespace tiny_skia {

void Blitter::unreachable() const { std::abort(); }

void Blitter::blitH(std::uint32_t x, std::uint32_t y, LengthU32 width) {
  (void)x;
  (void)y;
  (void)width;
  unreachable();
}

void Blitter::blitAntiH(std::uint32_t x, std::uint32_t y, std::span<std::uint8_t> alpha,
                        std::span<AlphaRun> runs) {
  (void)x;
  (void)y;
  (void)alpha;
  (void)runs;
  unreachable();
}

void Blitter::blitV(std::uint32_t x, std::uint32_t y, LengthU32 height, AlphaU8 alpha) {
  (void)x;
  (void)y;
  (void)height;
  (void)alpha;
  unreachable();
}

void Blitter::blitAntiH2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) {
  (void)x;
  (void)y;
  (void)alpha0;
  (void)alpha1;
  unreachable();
}

void Blitter::blitAntiV2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) {
  (void)x;
  (void)y;
  (void)alpha0;
  (void)alpha1;
  unreachable();
}

void Blitter::blitRect(const ScreenIntRect& rect) {
  (void)rect;
  unreachable();
}

void Blitter::blitMask(const Mask& mask, const ScreenIntRect& clip) {
  (void)mask;
  (void)clip;
  unreachable();
}

}  // namespace tiny_skia
