#pragma once
/// @file

#include <ostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "donner/backends/tiny_skia_cpp/Color.h"
#include "donner/backends/tiny_skia_cpp/Expected.h"
#include "donner/backends/tiny_skia_cpp/Pixmap.h"
#include "donner/backends/tiny_skia_cpp/Transform.h"
#include "donner/base/Vector2.h"

namespace donner::backends::tiny_skia_cpp {

/** Behavior outside of the gradient interval. */
enum class SpreadMode : uint8_t {
  kPad,
  kRepeat,
  kReflect,
};

inline std::ostream& operator<<(std::ostream& os, SpreadMode value) {
  switch (value) {
    case SpreadMode::kPad: return os << "SpreadMode::kPad";
    case SpreadMode::kRepeat: return os << "SpreadMode::kRepeat";
    case SpreadMode::kReflect: return os << "SpreadMode::kReflect";
  }
  return os;
}

/** A stop in a gradient ramp. Positions are normalized to [0, 1]. */
struct GradientStop {
  float position = 0.0f;
  Color color;
};

/** Normalized gradient data mirroring tiny-skia's stop handling. */
struct GradientData {
  std::vector<GradientStop> stops;
  bool colorsAreOpaque = false;
  bool hasUniformStops = false;
};

/** A solid-color shader. */
struct SolidColorShader {
  Color color;
};

/** Linear gradient shader parameters. */
struct LinearGradientShader {
  Vector2d start;
  Vector2d end;
  SpreadMode spreadMode = SpreadMode::kPad;
  Transform transform;
  GradientData gradient;
};

/** Radial gradient shader parameters. */
struct RadialGradientShader {
  Vector2d start;
  Vector2d end;
  float radius = 0.0f;
  SpreadMode spreadMode = SpreadMode::kPad;
  Transform transform;
  GradientData gradient;
};

/** Image sampling quality for pattern shaders. */
enum class FilterQuality : uint8_t {
  kNearest,
  kBilinear,
  kBicubic,
};

inline std::ostream& operator<<(std::ostream& os, FilterQuality value) {
  switch (value) {
    case FilterQuality::kNearest: return os << "FilterQuality::kNearest";
    case FilterQuality::kBilinear: return os << "FilterQuality::kBilinear";
    case FilterQuality::kBicubic: return os << "FilterQuality::kBicubic";
  }
  return os;
}

/** Pattern shader parameters that sample from an existing pixmap. */
struct PatternShader {
  const Pixmap* pixmap = nullptr;
  SpreadMode spreadMode = SpreadMode::kPad;
  FilterQuality filterQuality = FilterQuality::kNearest;
  float opacity = 1.0f;
  Transform transform;
};

/** Supported shader kinds. */
enum class ShaderKind : uint8_t {
  kSolidColor,
  kLinearGradient,
  kRadialGradient,
  kPattern,
};

/** Variant wrapper for shader definitions. */
class Shader {
public:
  Shader() = default;

  /// Creates a solid-color shader.
  static Shader MakeSolidColor(Color color);

  /**
   * Creates a linear gradient shader.
   *
   * @param start Starting point of the gradient line.
   * @param end End point of the gradient line.
   * @param stops Gradient stops. Must contain at least two entries.
   * @param spread Behavior outside the 0..1 interval.
   * @param transform Local transform applied to the gradient.
   */
  static Expected<Shader, std::string> MakeLinearGradient(Vector2d start, Vector2d end,
                                                          std::vector<GradientStop> stops,
                                                          SpreadMode spread,
                                                          const Transform& transform = Transform());

  /**
   * Creates a radial gradient shader.
   *
   * @param start Focal point of the gradient.
   * @param end Center of the outer circle.
   * @param radius Outer radius of the gradient. Must be positive.
   * @param stops Gradient stops. Must contain at least two entries.
   * @param spread Behavior outside the 0..1 interval.
   * @param transform Local transform applied to the gradient.
   */
  static Expected<Shader, std::string> MakeRadialGradient(Vector2d start, Vector2d end,
                                                          float radius,
                                                          std::vector<GradientStop> stops,
                                                          SpreadMode spread,
                                                          const Transform& transform = Transform());

  /**
   * Creates a pattern shader that samples from a pixmap.
   *
   * @param pixmap Source image to sample.
   * @param spread Behavior outside the image bounds.
   * @param quality Sampling quality used when transforming the image.
   * @param opacity Optional opacity multiplier clamped to [0, 1].
   * @param transform Local transform applied to the pattern.
   */
  static Expected<Shader, std::string> MakePattern(const Pixmap& pixmap, SpreadMode spread,
                                                   FilterQuality quality, float opacity,
                                                   const Transform& transform = Transform());

  /// Returns the shader type.
  ShaderKind kind() const { return kind_; }

  /// Returns the solid-color payload. Only valid when kind() == kSolidColor.
  const SolidColorShader& solidColor() const { return std::get<SolidColorShader>(data_); }

  /// Returns the linear gradient payload. Only valid when kind() == kLinearGradient.
  const LinearGradientShader& linearGradient() const {
    return std::get<LinearGradientShader>(data_);
  }

  /// Returns the radial gradient payload. Only valid when kind() == kRadialGradient.
  const RadialGradientShader& radialGradient() const {
    return std::get<RadialGradientShader>(data_);
  }

  /// Returns the pattern payload. Only valid when kind() == kPattern.
  const PatternShader& pattern() const { return std::get<PatternShader>(data_); }

private:
  explicit Shader(SolidColorShader shader);
  explicit Shader(LinearGradientShader shader);
  explicit Shader(RadialGradientShader shader);
  explicit Shader(PatternShader shader);

  static Expected<GradientData, std::string> NormalizeStops(std::vector<GradientStop> stops);

  ShaderKind kind_ = ShaderKind::kSolidColor;
  std::variant<SolidColorShader, LinearGradientShader, RadialGradientShader, PatternShader> data_{
      SolidColorShader{}};
};

/**
 * Compiled shader sampling context.
 *
 * Precomputes invariant values (transforms, deltas) so shader evaluation can be repeated for many
 * positions without re-validating inputs.
 */
class ShaderContext {
public:
  ShaderContext() = default;

  /// Creates a sampling context from a validated shader.
  static Expected<ShaderContext, std::string> Create(const Shader& shader);

  /// Samples the shader at the given position in device space.
  Color sample(const Vector2d& position) const;

  /// Samples a horizontal span for linear gradients using incremental evaluation when possible.
  /// Returns true if the shader is a linear gradient and @p outColors was populated.
  bool sampleLinearSpan(int x, int y, int width, std::vector<Color>& outColors) const;

private:
  explicit ShaderContext(Shader shader, Transform inverseTransform);

  Color sampleLinear(const Vector2d& position) const;
  void sampleLinearFastPath(const Vector2d& start, const Vector2d& step, int width,
                            std::vector<Color>& outColors) const;
#if defined(DONNER_ENABLE_TINY_SKIA_AVX2) && defined(__AVX2__)
  bool sampleLinearFastPathAvx2(const Vector2d& start, const Vector2d& step, int width,
                                std::vector<Color>& outColors) const;
#endif
#if defined(DONNER_ENABLE_TINY_SKIA_SSE2) && defined(__SSE2__)
  bool sampleLinearFastPathSimd(const Vector2d& start, const Vector2d& step, int width,
                                std::vector<Color>& outColors) const;
#endif
#if defined(DONNER_ENABLE_TINY_SKIA_NEON) && defined(__ARM_NEON)
  bool sampleLinearFastPathNeon(const Vector2d& start, const Vector2d& step, int width,
                                std::vector<Color>& outColors) const;
#endif
  Color sampleRadial(const Vector2d& position) const;
  Color samplePattern(const Vector2d& position) const;
  static Color sampleGradient(const GradientData& gradient, float t);
  static float applySpread(float t, SpreadMode spreadMode);
  Color sampleNearest(const Vector2d& local) const;
  Color sampleBilinear(const Vector2d& local) const;
  Color sampleBicubic(const Vector2d& local) const;
  Color sampleWithSpread(double x, double y) const;
  static double applySpreadToCoordinate(double coordinate, double extent, SpreadMode spreadMode);

  Shader shader_;
  Transform inverseTransform_;
  Vector2d linearDelta_;
  double linearLengthSquared_ = 0.0;

  Vector2d radialDelta_;
  double radialRadius_ = 0.0;
  double radialA_ = 0.0;

  FilterQuality filterQuality_ = FilterQuality::kNearest;
  const Pixmap* patternPixmap_ = nullptr;
};

}  // namespace donner::backends::tiny_skia_cpp
