#pragma once

/// @file Lighting.h
/// @brief SVG feDiffuseLighting and feSpecularLighting filter operations.

#include <array>
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

  // User-space positions for spot light cone angle computation.
  // The cone angle and exponent are computed in user space to correctly handle non-uniform
  // transforms (skew, rotation).  Device-space positions above are used for N·L shading.
  double userX = 0.0;
  double userY = 0.0;
  double userZ = 0.0;
  double userPointsAtX = 0.0;
  double userPointsAtY = 0.0;
  double userPointsAtZ = 0.0;
};

/// Parameters for diffuse lighting.
struct DiffuseLightingParams {
  double surfaceScale = 1.0;
  double diffuseConstant = 1.0;
  double lightR = 1.0;  ///< Light color red (0..1).
  double lightG = 1.0;  ///< Light color green (0..1).
  double lightB = 1.0;  ///< Light color blue (0..1).
  LightSourceParams light;

  /// Inverse transform mapping pixel coordinates to user/filter space.
  /// ux = pixelToUser[0]*px + pixelToUser[1]*py + pixelToUser[2]
  /// uy = pixelToUser[3]*px + pixelToUser[4]*py + pixelToUser[5]
  std::array<double, 6> pixelToUser = {1, 0, 0, 0, 1, 0};

  /// True if the device-from-filter transform has shear (non-conformal).
  bool hasShear = false;
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

  /// Inverse transform mapping pixel coordinates to user/filter space.
  std::array<double, 6> pixelToUser = {1, 0, 0, 0, 1, 0};

  /// True if the device-from-filter transform has shear (non-conformal).
  bool hasShear = false;
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
