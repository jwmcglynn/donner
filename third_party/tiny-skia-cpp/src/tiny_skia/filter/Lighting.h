#pragma once

/// @file Lighting.h
/// @brief SVG feDiffuseLighting and feSpecularLighting filter operations.

#include <cstdint>
#include <optional>

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace tiny_skia::filter {

/// Light source type.
enum class LightType : std::uint8_t { Distant, Point, Spot };

/// Light source parameters.
struct LightSourceParams {
  LightType type = LightType::Distant;

  // feDistantLight
  double azimuth = 0.0;    ///< Angle in XY plane (degrees).
  double elevation = 0.0;  ///< Angle above XY plane (degrees).

  // fePointLight / feSpotLight
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;

  // feSpotLight
  double pointsAtX = 0.0;
  double pointsAtY = 0.0;
  double pointsAtZ = 0.0;
  double spotExponent = 1.0;
  std::optional<double> limitingConeAngle;
};

/// Parameters for diffuse lighting.
struct DiffuseLightingParams {
  double surfaceScale = 1.0;
  double diffuseConstant = 1.0;
  double lightR = 1.0;  ///< Light color red (0..1).
  double lightG = 1.0;  ///< Light color green (0..1).
  double lightB = 1.0;  ///< Light color blue (0..1).
  LightSourceParams light;
};

/// Parameters for specular lighting.
struct SpecularLightingParams {
  double surfaceScale = 1.0;
  double specularConstant = 1.0;
  double specularExponent = 1.0;
  double lightR = 1.0;
  double lightG = 1.0;
  double lightB = 1.0;
  LightSourceParams light;
};

/// Apply diffuse lighting to the input pixmap's alpha channel as a bump map.
/// Output is written to dst (which must be same size as src).
void diffuseLighting(const Pixmap& src, Pixmap& dst, const DiffuseLightingParams& params);

/// Apply specular lighting to the input pixmap's alpha channel as a bump map.
/// Output is written to dst (which must be same size as src).
void specularLighting(const Pixmap& src, Pixmap& dst, const SpecularLightingParams& params);

/// Float-precision version of diffuseLighting.
void diffuseLighting(const FloatPixmap& src, FloatPixmap& dst, const DiffuseLightingParams& params);

/// Float-precision version of specularLighting.
void specularLighting(const FloatPixmap& src, FloatPixmap& dst,
                      const SpecularLightingParams& params);

}  // namespace tiny_skia::filter
