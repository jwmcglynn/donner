#pragma once

/// @file shaders/Mod.h
/// @brief Shader variant type and dispatch utilities.

#include <variant>

#include "tiny_skia/shaders/Gradient.h"
#include "tiny_skia/shaders/LinearGradient.h"
#include "tiny_skia/shaders/Pattern.h"
#include "tiny_skia/shaders/RadialGradient.h"
#include "tiny_skia/shaders/SweepGradient.h"

namespace tiny_skia {

/// Paint shader source — a solid color or one of the gradient/pattern types.
using Shader = std::variant<Color, LinearGradient, SweepGradient, RadialGradient, Pattern>;

/// @internal
[[nodiscard]] bool isShaderOpaque(const Shader& shader);

/// @internal
[[nodiscard]] bool pushShaderStages(const Shader& shader, ColorSpace cs,
                                    pipeline::RasterPipelineBuilder& p);

/// @internal
void transformShader(Shader& shader, const Transform& ts);

/// @internal
void applyShaderOpacity(Shader& shader, float opacity);

}  // namespace tiny_skia
