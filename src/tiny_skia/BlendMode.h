#pragma once

/// @file BlendMode.h
/// @brief Porter-Duff and advanced blend modes.

#include <optional>

#include "tiny_skia/Color.h"

namespace tiny_skia {

/// Blend mode for compositing source over destination.
/// Includes Porter-Duff modes and advanced (separable/non-separable) blend modes.
enum class BlendMode {
  // Porter-Duff modes.
  Clear,           ///< Output is transparent.
  Source,          ///< Output is source only.
  Destination,     ///< Output is destination only.
  SourceOver,      ///< Source over destination (default).
  DestinationOver, ///< Destination over source.
  SourceIn,        ///< Source where destination is opaque.
  DestinationIn,   ///< Destination where source is opaque.
  SourceOut,       ///< Source where destination is transparent.
  DestinationOut,  ///< Destination where source is transparent.
  SourceAtop,      ///< Source atop destination.
  DestinationAtop, ///< Destination atop source.
  Xor,             ///< Source XOR destination.
  Plus,            ///< Sum (clamped).
  Modulate,        ///< Component-wise multiply.
  // Advanced blend modes.
  Screen,          ///< Inverse multiply.
  Overlay,         ///< Multiply or screen based on destination.
  Darken,          ///< Minimum of source and destination.
  Lighten,         ///< Maximum of source and destination.
  ColorDodge,      ///< Brighten destination toward source.
  ColorBurn,       ///< Darken destination toward source.
  HardLight,       ///< Multiply or screen based on source.
  SoftLight,       ///< Soft version of hard light.
  Difference,      ///< Absolute difference.
  Exclusion,       ///< Similar to Difference but lower contrast.
  Multiply,        ///< Component-wise multiply (with alpha handling).
  Hue,             ///< Source hue, destination saturation and luminosity.
  Saturation,      ///< Source saturation, destination hue and luminosity.
  Color,           ///< Source hue and saturation, destination luminosity.
  Luminosity,      ///< Source luminosity, destination hue and saturation.
};

/// @internal
[[nodiscard]] bool shouldPreScaleCoverage(BlendMode blendMode);
/// @internal
[[nodiscard]] std::optional<pipeline::Stage> toStage(BlendMode blendMode);

}  // namespace tiny_skia
