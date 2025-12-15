#pragma once
/// @file

#include <optional>
#include <vector>

#include "donner/backends/tiny_skia_cpp/BlendMode.h"
#include "donner/backends/tiny_skia_cpp/Color.h"
#include "donner/backends/tiny_skia_cpp/Expected.h"
#include "donner/backends/tiny_skia_cpp/Pixmap.h"
#include "donner/backends/tiny_skia_cpp/Shader.h"
#include "donner/base/Vector2.h"

namespace donner::backends::tiny_skia_cpp {

/** Paint parameters describing how geometry should be filled. */
struct Paint {
  Color color = Color::RGB(0, 0, 0);
  float opacity = 1.0f;
  BlendMode blendMode = BlendMode::kSourceOver;
  std::optional<Shader> shader;
  bool antiAlias = true;
};

/** Parameters controlling how pixmap sources are sampled. */
struct PixmapPaint {
  float opacity = 1.0f;
  BlendMode blendMode = BlendMode::kSourceOver;
  FilterQuality quality = FilterQuality::kNearest;
};

/**
 * Precomputed paint state that can shade and blend pixels efficiently.
 */
class PaintContext {
public:
  PaintContext() = default;

  /// Validates the paint and precomputes shader state.
  static Expected<PaintContext, std::string> Create(const Paint& paint);

  /// Samples the paint at the given device-space position and applies opacity.
  Color shade(const Vector2d& position) const;

  /// Attempts to shade a horizontal span using a shader fast path. Returns false when unavailable.
  bool shadeLinearSpan(int x, int y, int width, std::vector<Color>& outColors) const;

  /// Returns the configured blend mode.
  BlendMode blendMode() const { return paint_.blendMode; }

  /// Returns true when a shader will be sampled during shading.
  bool hasShader() const { return shaderContext_.has_value(); }

  /// Returns the base paint color.
  Color color() const { return paint_.color; }

  /// Returns the clamped opacity.
  float opacity() const { return opacity_; }

  /// Applies opacity to the provided color.
  Color applyOpacity(Color color) const;

private:
  PaintContext(Paint paint, std::optional<ShaderContext> shaderContext, float clampedOpacity);

  Paint paint_;
  std::optional<ShaderContext> shaderContext_;
  float opacity_ = 1.0f;
};

/**
 * Shades and blends a horizontal span of pixels using the provided paint context.
 *
 * @param pixmap Destination surface to modify.
 * @param x Starting x coordinate in pixels.
 * @param y Row index to update.
 * @param width Number of pixels to shade.
 * @param paintContext Precomputed paint data used for shading and blending.
 */
void BlendSpan(Pixmap& pixmap, int x, int y, int width, const PaintContext& paintContext);

/**
 * Shades and blends a horizontal span using per-pixel coverage values.
 *
 * @param pixmap Destination surface to modify.
 * @param x Starting x coordinate in pixels.
 * @param y Row index to update.
 * @param coverage Pointer to coverage values, one per pixel, sized to @p width.
 * @param width Number of pixels to shade.
 * @param paintContext Precomputed paint data used for shading and blending.
 */
void BlendMaskSpan(Pixmap& pixmap, int x, int y, const uint8_t* coverage, int width,
                   const PaintContext& paintContext);

}  // namespace donner::backends::tiny_skia_cpp
