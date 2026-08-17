#pragma once

/// @file Paint.h
/// @brief Paint configuration for drawing operations.

#include <cstdint>

#include "tiny_skia/BlendMode.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/shaders/Shaders.h"

namespace tiny_skia {

/// Controls how a shape is painted (shader, blend mode, anti-aliasing).
struct Paint {
  /// Paint shader source. Default: solid black.
  Shader shader = Color::black;

  /// Blend mode. Default: SourceOver.
  BlendMode blendMode = BlendMode::SourceOver;

  /// Enable anti-aliased rendering. Default: true.
  bool antiAlias = true;

  /// Colorspace for gamma-correct blending. Default: Linear.
  ColorSpace colorspace = ColorSpace::Linear;

  /// Force the high-quality (highp) rendering pipeline. Default: false.
  bool forceHqPipeline = false;

  /// Sets the shader to a solid color.
  void setColor(const Color& color) { shader = color; }

  /// Sets the shader to a solid color from 8-bit RGBA components.
  void setColorRgba8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    setColor(Color::fromRgba8(r, g, b, a));
  }

  /// Returns true if the shader is a solid color.
  [[nodiscard]] bool isSolidColor() const { return std::holds_alternative<Color>(shader); }
};

}  // namespace tiny_skia
