#pragma once

/// @file shaders/Pattern.h
/// @brief Pixmap-based pattern shader and PixmapPaint settings.

#include "tiny_skia/BlendMode.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/pipeline/Pipeline.h"

namespace tiny_skia {

/// Filter quality for pattern/pixmap sampling.
enum class FilterQuality {
  Nearest,  ///< Nearest-neighbor (pixelated).
  Bilinear, ///< Bilinear interpolation.
  Bicubic,  ///< Bicubic interpolation (highest quality).
};

/// Paint settings for Painter::drawPixmap.
struct PixmapPaint {
  float opacity = 1.0f;                          ///< Opacity [0,1].
  BlendMode blendMode = BlendMode::SourceOver;    ///< Blend mode.
  FilterQuality quality = FilterQuality::Nearest;  ///< Sampling filter.

  /// Pin this blit to the float raster pipeline instead of letting the blitter
  /// choose the 8-bit fixed-point one.
  ///
  /// The 8-bit compose path drifts through Painter::drawPixmap: a fully opaque
  /// source pixel can land in the destination at alpha 250 instead of 255,
  /// with the premultiplied RGB preserved. On an intermediate surface that
  /// drift is absorbed by the next composite, but on a surface that is read
  /// back or presented it is visible as a nominally opaque region reporting
  /// partial coverage. Callers compositing into such a surface set this so the
  /// drift cannot reach output. It is a workaround for that arithmetic, not a
  /// quality preference, and should be dropped once the 8-bit compose path
  /// stops losing alpha.
  bool forceHqPipeline = false;
};

/// Pixmap-based pattern shader.
///
/// Tiles or clamps a PixmapView according to the spread mode.
class Pattern {
 public:
  Pattern(PixmapView pixmap, SpreadMode spreadMode, FilterQuality quality, float opacity,
          Transform transform);

  [[nodiscard]] bool isOpaque() const;

  /// @internal
  [[nodiscard]] bool pushStages(ColorSpace cs, pipeline::RasterPipelineBuilder& p) const;

  /// @internal
  PixmapView pixmap_;
  /// @internal
  NormalizedF32 opacity_;
  /// @internal
  Transform transform_;

 private:
  FilterQuality quality_;
  SpreadMode spreadMode_;
};

}  // namespace tiny_skia
