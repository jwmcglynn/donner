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

  /// Never equal, deliberately.
  ///
  /// A pattern borrows its pixels through a view, so two patterns that describe the same
  /// tile today can describe different pixels a moment later without either object changing.
  /// Equality exists so a caller can ask "is this the same paint I saw before, such that work
  /// derived from it is still valid", and for a borrowed-pixel shader the honest answer is
  /// always no. Reporting equal would let a caller reuse output built from pixels that have
  /// since been overwritten.
  friend bool operator==(const Pattern&, const Pattern&) { return false; }

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
