#pragma once

#include <optional>

#include "tiny_skia/BlendMode.h"
#include "tiny_skia/Blitter.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Mask.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/pipeline/Pipeline.h"

namespace tiny_skia {
struct Paint;
}

namespace tiny_skia::pipeline {

class RasterPipelineBlitter final : public tiny_skia::Blitter {
 public:
  /// Creates a blitter from a solid color.
  static std::optional<RasterPipelineBlitter> create(PremultipliedColorU8 color,
                                                     MutableSubPixmapView* pixmap,
                                                     BlendMode blendMode = BlendMode::SourceOver,
                                                     std::optional<SubMaskView> mask = std::nullopt);

  /// Creates a blitter from a Paint (with Shader support).
  static std::optional<RasterPipelineBlitter> create(const tiny_skia::Paint& paint,
                                                     std::optional<SubMaskView> mask,
                                                     MutableSubPixmapView* pixmap);

  static std::optional<RasterPipelineBlitter> createMask(MutableSubPixmapView* pixmap);

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

  [[nodiscard]] bool isMaskOnly() const { return isMaskOnly_; }

 private:
  RasterPipelineBlitter(MutableSubPixmapView* pixmap, bool isMaskOnly,
                        std::optional<PremultipliedColorU8> memsetColor,
                        std::optional<PremultipliedColorU8> solidSrcOverColor,
                        std::optional<SubMaskView> mask, Pixmap pixmapSrcStorage,
                        RasterPipeline blitAntiHRp, RasterPipeline blitRectRp,
                        RasterPipeline blitMaskRp);

  MutableSubPixmapView* pixmap_ = nullptr;
  bool isMaskOnly_ = false;
  std::optional<PremultipliedColorU8> memsetColor_;
  /// Premultiplied color for solid SourceOver fast paths (blitAntiH2/blitV).
  std::optional<PremultipliedColorU8> solidSrcOverColor_;
  std::optional<SubMaskView> mask_;
  Pixmap pixmapSrcStorage_;
  RasterPipeline blitAntiHRp_;
  RasterPipeline blitRectRp_;
  RasterPipeline blitMaskRp_;
};

}  // namespace tiny_skia::pipeline
