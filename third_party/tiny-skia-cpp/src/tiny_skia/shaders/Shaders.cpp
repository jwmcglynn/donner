#include "tiny_skia/shaders/Shaders.h"

#include "tiny_skia/Math.h"

namespace tiny_skia {

bool isShaderOpaque(const Shader& shader) {
  return std::visit(
      [](const auto& s) -> bool {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, Color>) {
          return s.isOpaque();
        } else if constexpr (std::is_same_v<T, LinearGradient> ||
                             std::is_same_v<T, SweepGradient> ||
                             std::is_same_v<T, RadialGradient>) {
          return s.isOpaque();
        } else if constexpr (std::is_same_v<T, Pattern>) {
          return s.isOpaque();
        }
        return false;
      },
      shader);
}

bool pushShaderStages(const Shader& shader, ColorSpace cs, pipeline::RasterPipelineBuilder& p) {
  return std::visit(
      [&](const auto& s) -> bool {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, Color>) {
          const auto premult = expandColor(cs, s).premultiply();
          p.pushUniformColor(premult);
          return true;
        } else if constexpr (std::is_same_v<T, LinearGradient> ||
                             std::is_same_v<T, SweepGradient> ||
                             std::is_same_v<T, RadialGradient>) {
          return s.pushStages(cs, p);
        } else if constexpr (std::is_same_v<T, Pattern>) {
          return s.pushStages(cs, p);
        }
        return false;
      },
      shader);
}

void transformShader(Shader& shader, const Transform& ts) {
  std::visit(
      [&](auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, Color>) {
          // SolidColor is transform-invariant.
        } else if constexpr (std::is_same_v<T, LinearGradient> ||
                             std::is_same_v<T, SweepGradient> ||
                             std::is_same_v<T, RadialGradient>) {
          s.base_.transform = s.base_.transform.postConcat(ts);
        } else if constexpr (std::is_same_v<T, Pattern>) {
          s.transform_ = s.transform_.postConcat(ts);
        }
      },
      shader);
}

void applyShaderOpacity(Shader& shader, float opacity) {
  std::visit(
      [&](auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, Color>) {
          s.applyOpacity(opacity);
        } else if constexpr (std::is_same_v<T, LinearGradient> ||
                             std::is_same_v<T, SweepGradient> ||
                             std::is_same_v<T, RadialGradient>) {
          s.base_.applyOpacity(opacity);
        } else if constexpr (std::is_same_v<T, Pattern>) {
          s.opacity_ = NormalizedF32::newClamped(s.opacity_.get() * bound(0.0f, opacity, 1.0f));
        }
      },
      shader);
}

}  // namespace tiny_skia
