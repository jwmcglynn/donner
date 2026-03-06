#pragma once

/// @file pipeline/Mod.h
/// @brief Rendering pipeline types: SpreadMode, Stage, RasterPipeline.
///
/// Most types in this file are internal to the rendering engine.
/// The main public type is SpreadMode.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "tiny_skia/Transform.h"

namespace tiny_skia {

class Color;
class PremultipliedColor;
class PixmapView;
class ScreenIntRect;
struct MutableSubPixmapView;

/// How gradient/pattern colors extend beyond the [0,1] range.
enum class SpreadMode {
  Pad,     ///< Clamp to the nearest edge color.
  Reflect, ///< Mirror the gradient.
  Repeat,  ///< Tile the gradient.
};

/// @internal
namespace pipeline {

/// @internal
namespace highp {
struct Pipeline;
}

/// @internal
namespace lowp {
struct Pipeline;
}

/// @internal
/// Pipeline stage identifier.
enum class Stage : std::uint8_t {
  MoveSourceToDestination,
  MoveDestinationToSource,
  Clamp0,
  ClampA,
  Premultiply,
  UniformColor,
  SeedShader,
  LoadDestination,
  Store,
  LoadDestinationU8,
  StoreU8,
  Gather,
  LoadMaskU8,
  MaskU8,
  ScaleU8,
  LerpU8,
  Scale1Float,
  Lerp1Float,
  DestinationAtop,
  DestinationIn,
  DestinationOut,
  DestinationOver,
  SourceAtop,
  SourceIn,
  SourceOut,
  SourceOver,
  Clear,
  Modulate,
  Multiply,
  Plus,
  Screen,
  Xor,
  ColorBurn,
  ColorDodge,
  Darken,
  Difference,
  Exclusion,
  HardLight,
  Lighten,
  Overlay,
  SoftLight,
  Hue,
  Saturation,
  Color,
  Luminosity,
  SourceOverRgba,
  Transform,
  Reflect,
  Repeat,
  Bilinear,
  Bicubic,
  PadX1,
  ReflectX1,
  RepeatX1,
  Gradient,
  EvenlySpaced2StopGradient,
  XYToUnitAngle,
  XYToRadius,
  XYTo2PtConicalFocalOnCircle,
  XYTo2PtConicalWellBehaved,
  XYTo2PtConicalSmaller,
  XYTo2PtConicalGreater,
  XYTo2PtConicalStrip,
  Mask2PtConicalNan,
  Mask2PtConicalDegenerates,
  ApplyVectorMask,
  Alter2PtConicalCompensateFocal,
  Alter2PtConicalUnswap,
  NegateX,
  ApplyConcentricScaleBias,
  GammaExpand2,
  GammaExpandDestination2,
  GammaCompress2,
  GammaExpand22,
  GammaExpandDestination22,
  GammaCompress22,
  GammaExpandSrgb,
  GammaExpandDestinationSrgb,
  GammaCompressSrgb,
};

/// @internal
inline constexpr std::size_t kStagesCount = 1 + static_cast<std::size_t>(Stage::GammaCompressSrgb);
/// @internal
inline constexpr std::size_t kMaxStages = 32;

/// @internal
struct AAMaskCtx {
  std::array<std::uint8_t, 2> pixels = {0, 0};
  std::uint32_t stride = 0;
  std::size_t shift = 0;

  [[nodiscard]] std::array<std::uint8_t, 2> copyAtXY(std::size_t dx, std::size_t dy,
                                                     std::size_t tail) const {
    const auto base = static_cast<std::size_t>(stride) * dy + dx;
    if (base < shift) {
      return {0, 0};
    }
    const auto offset = base - shift;
    if (offset == 0 && tail == 1) {
      return {pixels[0], 0};
    }
    if (offset == 0 && tail == 2) {
      return {pixels[0], pixels[1]};
    }
    if (offset == 1 && tail == 1) {
      return {pixels[1], 0};
    }
    return {0, 0};
  }
};

/// @internal
struct MaskCtx {
  const std::uint8_t* data = nullptr;
  std::uint32_t realWidth = 0;

  [[nodiscard]] std::size_t byteOffset(std::size_t dx, std::size_t dy) const {
    return offset(dx, dy);
  }

 private:
  [[nodiscard]] constexpr std::size_t offset(std::size_t dx, std::size_t dy) const {
    return static_cast<std::size_t>(realWidth) * dy + dx;
  }
};

/// @internal
struct SamplerCtx {
  SpreadMode spreadMode = SpreadMode::Pad;
  float invWidth = 0.0f;
  float invHeight = 0.0f;
};

/// @internal
struct UniformColorCtx {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 0.0f;
  std::array<std::uint16_t, 4> rgba = {0, 0, 0, 0};
};

/// @internal
struct GradientColor {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 0.0f;

  constexpr bool operator==(const GradientColor&) const = default;

  [[nodiscard]] static constexpr GradientColor newFromRGBA(float r, float g, float b, float a) {
    return GradientColor{r, g, b, a};
  }
};

/// @internal
struct EvenlySpaced2StopGradientCtx {
  GradientColor factor{};
  GradientColor bias{};
};

/// @internal
struct TwoPointConicalGradientCtx {
  std::array<std::uint32_t, 8> mask = {};
  float p0 = 0.0f;
  float p1 = 0.0f;
};

/// @internal
struct TileCtx {
  float scale = 0.0f;
  float invScale = 0.0f;
};

/// @internal
struct Context {
  float currentCoverage = 0.0f;
  SamplerCtx sampler;
  UniformColorCtx uniformColor;
  EvenlySpaced2StopGradientCtx evenlySpaced2StopGradient;
  struct GradientCtx {
    std::size_t len = 0;
    std::vector<GradientColor> factors;
    std::vector<GradientColor> biases;
    std::vector<float> tValues;

    void pushConstColor(GradientColor color) {
      factors.push_back(GradientColor{0.0f, 0.0f, 0.0f, 0.0f});
      biases.push_back(color);
    }
  } gradient;

  TwoPointConicalGradientCtx twoPointConicalGradient;
  TileCtx limitX;
  TileCtx limitY;
  Transform transform;
};

/// @internal
class RasterPipeline {
 public:
  enum class Kind {
    High,
    Low,
  };

  RasterPipeline() = default;
  RasterPipeline(Kind kind, Context context, const std::array<Stage, kMaxStages>& stages,
                 std::size_t stageCount);

  [[nodiscard]] Kind kind() const { return kind_; }

  Context& ctx() { return ctx_; }
  [[nodiscard]] const Context& ctx() const { return ctx_; }

  void run(const ScreenIntRect& rect, const AAMaskCtx& aaMaskCtx, MaskCtx maskCtx,
           const PixmapView& pixmapSrc, MutableSubPixmapView* pixmapDst);

  [[nodiscard]] std::size_t stageCount() const { return stageCount_; }

 private:
  using HighpStageFn = void (*)(highp::Pipeline&);
  using LowpStageFn = void (*)(lowp::Pipeline&);

  void initializeFunctions();

  Kind kind_ = Kind::High;
  Context ctx_{};
  std::array<Stage, kMaxStages> stages_ = {};
  std::size_t stageCount_ = 0;
  std::array<HighpStageFn, kMaxStages> highpFunctions_{};
  std::array<HighpStageFn, kMaxStages> highpTailFunctions_{};
  std::array<LowpStageFn, kMaxStages> lowpFunctions_{};
  std::array<LowpStageFn, kMaxStages> lowpTailFunctions_{};
};

/// @internal
class RasterPipelineBuilder {
 public:
  RasterPipelineBuilder() = default;

  void setForceHqPipeline(bool hq) { forceHqPipeline_ = hq; }

  void push(Stage stage) {
    if (stageCount_ >= kMaxStages) {
      return;
    }
    stages_[stageCount_++] = stage;
  }

  void pushTransform(const Transform& ts) {
    if (ts.isFinite() && !ts.isIdentity()) {
      push(Stage::Transform);
      ctx_.transform = ts;
    }
  }

  void pushUniformColor(const PremultipliedColor& c);

  [[nodiscard]] RasterPipeline compile();

  [[nodiscard]] Context& ctx() { return ctx_; }
  [[nodiscard]] const Context& ctx() const { return ctx_; }

 private:
  std::array<Stage, kMaxStages> stages_ = {};
  std::size_t stageCount_ = 0;
  bool forceHqPipeline_ = false;
  Context ctx_;
};

}  // namespace pipeline

}  // namespace tiny_skia
