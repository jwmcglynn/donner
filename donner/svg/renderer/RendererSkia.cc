#include "donner/svg/renderer/RendererSkia.h"

// Skia
#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPathMeasure.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPoint3.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkPerlinNoiseShader.h"
#include "include/pathops/SkPathOps.h"

#ifdef DONNER_USE_CORETEXT
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(DONNER_USE_FREETYPE)
#include "include/ports/SkFontMgr_empty.h"
#elif defined(DONNER_USE_FREETYPE_WITH_FONTCONFIG)
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#else
#error \
    "Neither DONNER_USE_CORETEXT, DONNER_USE_FREETYPE, nor DONNER_USE_FREETYPE_WITH_FONTCONFIG is defined"
#endif
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"
#include "donner/svg/text/TextLayoutParams.h"
// Donner
#include <map>
#include <numbers>

#include "donner/base/Length.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "embed_resources/PublicSansFont.h"

namespace donner::svg {

namespace {

TextLayoutParams toTextLayoutParams(const TextParams& params) {
  TextLayoutParams layoutParams;
  layoutParams.fontFamilies = params.fontFamilies;
  layoutParams.fontSize = params.fontSize;
  layoutParams.viewBox = params.viewBox;
  layoutParams.fontMetrics = params.fontMetrics;
  layoutParams.textAnchor = params.textAnchor;
  layoutParams.dominantBaseline = params.dominantBaseline;
  layoutParams.writingMode = params.writingMode;
  layoutParams.letterSpacingPx = params.letterSpacingPx;
  layoutParams.wordSpacingPx = params.wordSpacingPx;
  layoutParams.textLength = params.textLength;
  layoutParams.lengthAdjust = params.lengthAdjust;
  return layoutParams;
}

const Boxd kUnitPathBounds(Vector2d::Zero(), Vector2d(1, 1));

SkPoint toSkia(Vector2d value) {
  return SkPoint::Make(NarrowToFloat(value.x), NarrowToFloat(value.y));
}

SkMatrix toSkiaMatrix(const Transformd& transform) {
  return SkMatrix::MakeAll(NarrowToFloat(transform.data[0]),  // scaleX
                           NarrowToFloat(transform.data[2]),  // skewX
                           NarrowToFloat(transform.data[4]),  // transX
                           NarrowToFloat(transform.data[1]),  // skewY
                           NarrowToFloat(transform.data[3]),  // scaleY
                           NarrowToFloat(transform.data[5]),  // transY
                           0, 0, 1);
}

SkRect toSkia(const Boxd& box) {
  return SkRect::MakeLTRB(
      static_cast<SkScalar>(box.topLeft.x), static_cast<SkScalar>(box.topLeft.y),
      static_cast<SkScalar>(box.bottomRight.x), static_cast<SkScalar>(box.bottomRight.y));
}

Transformd toDonnerTransform(const SkMatrix& matrix) {
  Transformd transform;
  transform.data[0] = matrix.getScaleX();
  transform.data[1] = matrix.getSkewY();
  transform.data[2] = matrix.getSkewX();
  transform.data[3] = matrix.getScaleY();
  transform.data[4] = matrix.getTranslateX();
  transform.data[5] = matrix.getTranslateY();
  return transform;
}

SkColor toSkia(const css::RGBA rgba) {
  return SkColorSetARGB(rgba.a, rgba.r, rgba.g, rgba.b);
}

SkFontStyle toSkiaFontStyle(int fontWeight, FontStyle fontStyle, FontStretch fontStretch) {
  // Map FontStretch enum (1-9) to Skia width values (1-9, same range).
  const int skWidth = static_cast<int>(fontStretch);

  SkFontStyle::Slant skSlant;
  switch (fontStyle) {
    case FontStyle::Normal: skSlant = SkFontStyle::kUpright_Slant; break;
    case FontStyle::Italic: skSlant = SkFontStyle::kItalic_Slant; break;
    case FontStyle::Oblique: skSlant = SkFontStyle::kOblique_Slant; break;
  }

  return SkFontStyle(fontWeight, skWidth, skSlant);
}

SkPath toSkia(const PathSpline& spline);

PathSpline transformPathSpline(const PathSpline& spline, const Transformd& transform) {
  PathSpline result;
  const std::vector<Vector2d>& points = spline.points();

  for (const PathSpline::Command& command : spline.commands()) {
    switch (command.type) {
      case PathSpline::CommandType::MoveTo:
        result.moveTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case PathSpline::CommandType::LineTo:
        result.lineTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case PathSpline::CommandType::CurveTo:
        result.curveTo(transform.transformPosition(points[command.pointIndex]),
                       transform.transformPosition(points[command.pointIndex + 1]),
                       transform.transformPosition(points[command.pointIndex + 2]));
        break;
      case PathSpline::CommandType::ClosePath: result.closePath(); break;
    }
  }

  return result;
}

SkTileMode toSkia(GradientSpreadMethod spreadMethod);

/// Premultiply straight-alpha RGBA pixels for Skia consumption.
std::vector<std::uint8_t> PremultiplyRgba(std::span<const std::uint8_t> rgbaPixels) {
  std::vector<std::uint8_t> result(rgbaPixels.size());
  for (std::size_t i = 0; i + 3 < rgbaPixels.size(); i += 4) {
    const float a = rgbaPixels[i + 3] / 255.0f;
    result[i + 0] = static_cast<std::uint8_t>(rgbaPixels[i + 0] * a);
    result[i + 1] = static_cast<std::uint8_t>(rgbaPixels[i + 1] * a);
    result[i + 2] = static_cast<std::uint8_t>(rgbaPixels[i + 2] * a);
    result[i + 3] = rgbaPixels[i + 3];
  }
  return result;
}

void applyClipToCanvas(SkCanvas* canvas, const ResolvedClip& clip) {
  if (clip.clipRect.has_value()) {
    canvas->clipRect(toSkia(*clip.clipRect));
  }

  if (clip.clipPaths.empty()) {
    return;
  }

  const SkMatrix skUnitsTransform = toSkiaMatrix(clip.clipPathUnitsTransform);

  SkPath fullPath;
  SmallVector<SkPath, 5> layeredPaths;
  int currentLayer = 0;

  for (const PathShape& shape : clip.clipPaths) {
    SkPath skPath = toSkia(shape.path);
    skPath.setFillType(shape.fillRule == FillRule::EvenOdd ? SkPathFillType::kEvenOdd
                                                           : SkPathFillType::kWinding);
    skPath.transform(toSkiaMatrix(shape.entityFromParent) * skUnitsTransform);

    if (shape.layer > currentLayer) {
      layeredPaths.push_back(skPath);
      currentLayer = shape.layer;
      continue;
    } else if (shape.layer < currentLayer) {
      assert(!layeredPaths.empty());
      SkPath layerPath = layeredPaths[layeredPaths.size() - 1];
      layeredPaths.pop_back();
      layerPath.transform(toSkiaMatrix(shape.entityFromParent) * skUnitsTransform);
      Op(layerPath, skPath, kIntersect_SkPathOp, &skPath);
      currentLayer = shape.layer;

      if (currentLayer != 0) {
        layeredPaths.push_back(skPath);
        continue;
      }
    }

    SkPath& targetPath = layeredPaths.empty() ? fullPath : layeredPaths[layeredPaths.size() - 1];
    Op(targetPath, skPath, kUnion_SkPathOp, &targetPath);
  }

  canvas->clipPath(fullPath, SkClipOp::kIntersect, true);
}

bool graphUsesStandardInput(const components::FilterGraph& filterGraph,
                            components::FilterStandardInput input) {
  for (const components::FilterNode& node : filterGraph.nodes) {
    for (const components::FilterInput& nodeInput : node.inputs) {
      const auto* standardInput = std::get_if<components::FilterStandardInput>(&nodeInput.value);
      if (standardInput != nullptr && *standardInput == input) {
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Native Skia filter DAG builder — lowers the full FilterGraph to SkImageFilter.
// ---------------------------------------------------------------------------

/// Convert donner Blend::Mode to SkBlendMode.
SkBlendMode toSkiaBlendMode(components::filter_primitive::Blend::Mode mode) {
  using Mode = components::filter_primitive::Blend::Mode;
  switch (mode) {
    case Mode::Normal: return SkBlendMode::kSrcOver;
    case Mode::Multiply: return SkBlendMode::kMultiply;
    case Mode::Screen: return SkBlendMode::kScreen;
    case Mode::Darken: return SkBlendMode::kDarken;
    case Mode::Lighten: return SkBlendMode::kLighten;
    case Mode::Overlay: return SkBlendMode::kOverlay;
    case Mode::ColorDodge: return SkBlendMode::kColorDodge;
    case Mode::ColorBurn: return SkBlendMode::kColorBurn;
    case Mode::HardLight: return SkBlendMode::kHardLight;
    case Mode::SoftLight: return SkBlendMode::kSoftLight;
    case Mode::Difference: return SkBlendMode::kDifference;
    case Mode::Exclusion: return SkBlendMode::kExclusion;
    case Mode::Hue: return SkBlendMode::kHue;
    case Mode::Saturation: return SkBlendMode::kSaturation;
    case Mode::Color: return SkBlendMode::kColor;
    case Mode::Luminosity: return SkBlendMode::kLuminosity;
  }
  return SkBlendMode::kSrcOver;
}

/// Convert donner Composite::Operator to SkBlendMode (for non-arithmetic operators).
std::optional<SkBlendMode> compositeOpToBlendMode(
    components::filter_primitive::Composite::Operator op) {
  using Op = components::filter_primitive::Composite::Operator;
  switch (op) {
    case Op::Over: return SkBlendMode::kSrcOver;
    case Op::In: return SkBlendMode::kSrcIn;
    case Op::Out: return SkBlendMode::kSrcOut;
    case Op::Atop: return SkBlendMode::kSrcATop;
    case Op::Xor: return SkBlendMode::kXor;
    case Op::Lighter: return SkBlendMode::kPlus;
    case Op::Arithmetic: return std::nullopt;  // Uses SkImageFilters::Arithmetic instead.
  }
  return SkBlendMode::kSrcOver;
}

/// Convert donner GaussianBlur::EdgeMode to SkTileMode.
SkTileMode blurEdgeModeToTileMode(components::filter_primitive::GaussianBlur::EdgeMode edgeMode) {
  using EdgeMode = components::filter_primitive::GaussianBlur::EdgeMode;
  switch (edgeMode) {
    case EdgeMode::None: return SkTileMode::kDecal;
    case EdgeMode::Duplicate: return SkTileMode::kClamp;
    case EdgeMode::Wrap: return SkTileMode::kRepeat;
  }
  return SkTileMode::kDecal;
}

/// Convert donner ConvolveMatrix::EdgeMode to SkTileMode.
SkTileMode convolveEdgeModeToTileMode(
    components::filter_primitive::ConvolveMatrix::EdgeMode edgeMode) {
  using EdgeMode = components::filter_primitive::ConvolveMatrix::EdgeMode;
  switch (edgeMode) {
    case EdgeMode::Duplicate: return SkTileMode::kClamp;
    case EdgeMode::Wrap: return SkTileMode::kRepeat;
    case EdgeMode::None: return SkTileMode::kDecal;
  }
  return SkTileMode::kClamp;
}

/// Convert donner DisplacementMap::Channel to SkColorChannel.
SkColorChannel toSkiaColorChannel(components::filter_primitive::DisplacementMap::Channel channel) {
  using Channel = components::filter_primitive::DisplacementMap::Channel;
  switch (channel) {
    case Channel::R: return SkColorChannel::kR;
    case Channel::G: return SkColorChannel::kG;
    case Channel::B: return SkColorChannel::kB;
    case Channel::A: return SkColorChannel::kA;
  }
  return SkColorChannel::kA;
}

/// Build the 5×4 color matrix for feColorMatrix.
/// Returns a float[20] array suitable for SkColorFilters::Matrix().
std::array<float, 20> buildColorMatrix(const components::filter_primitive::ColorMatrix& cm) {
  std::array<float, 20> m{};
  switch (cm.type) {
    case components::filter_primitive::ColorMatrix::Type::Matrix: {
      for (std::size_t i = 0; i < std::min(cm.values.size(), std::size_t{20}); ++i) {
        m[i] = static_cast<float>(cm.values[i]);
      }
      break;
    }
    case components::filter_primitive::ColorMatrix::Type::Saturate: {
      const float s = cm.values.empty() ? 1.0f : static_cast<float>(cm.values[0]);
      // SVG saturate matrix
      m[0] = 0.213f + 0.787f * s;
      m[1] = 0.715f - 0.715f * s;
      m[2] = 0.072f - 0.072f * s;
      m[5] = 0.213f - 0.213f * s;
      m[6] = 0.715f + 0.285f * s;
      m[7] = 0.072f - 0.072f * s;
      m[10] = 0.213f - 0.213f * s;
      m[11] = 0.715f - 0.715f * s;
      m[12] = 0.072f + 0.928f * s;
      m[18] = 1.0f;
      break;
    }
    case components::filter_primitive::ColorMatrix::Type::HueRotate: {
      const double angle =
          (cm.values.empty() ? 0.0 : cm.values[0]) * std::numbers::pi_v<double> / 180.0;
      const float cosA = static_cast<float>(std::cos(angle));
      const float sinA = static_cast<float>(std::sin(angle));
      m[0] = 0.213f + cosA * 0.787f - sinA * 0.213f;
      m[1] = 0.715f - cosA * 0.715f - sinA * 0.715f;
      m[2] = 0.072f - cosA * 0.072f + sinA * 0.928f;
      m[5] = 0.213f - cosA * 0.213f + sinA * 0.143f;
      m[6] = 0.715f + cosA * 0.285f + sinA * 0.140f;
      m[7] = 0.072f - cosA * 0.072f - sinA * 0.283f;
      m[10] = 0.213f - cosA * 0.213f - sinA * 0.787f;
      m[11] = 0.715f - cosA * 0.715f + sinA * 0.715f;
      m[12] = 0.072f + cosA * 0.928f + sinA * 0.072f;
      m[18] = 1.0f;
      break;
    }
    case components::filter_primitive::ColorMatrix::Type::LuminanceToAlpha: {
      m[15] = 0.2126f;
      m[16] = 0.7152f;
      m[17] = 0.0722f;
      break;
    }
  }
  return m;
}

/// Build a 256-entry LUT for a single feComponentTransfer channel function.
void buildTransferTable(const components::filter_primitive::ComponentTransfer::Func& func,
                        uint8_t table[256]) {
  using FuncType = components::filter_primitive::ComponentTransfer::FuncType;
  switch (func.type) {
    case FuncType::Identity:
      for (int i = 0; i < 256; ++i) {
        table[i] = static_cast<uint8_t>(i);
      }
      break;
    case FuncType::Table: {
      const auto& v = func.tableValues;
      const int n = static_cast<int>(v.size());
      if (n < 2) {
        for (int i = 0; i < 256; ++i) {
          table[i] = static_cast<uint8_t>(i);
        }
        break;
      }
      for (int i = 0; i < 256; ++i) {
        const double c = static_cast<double>(i) / 255.0 * static_cast<double>(n - 1);
        const int k = std::min(static_cast<int>(c), n - 2);
        const double frac = c - k;
        const double val = v[static_cast<std::size_t>(k)] * (1.0 - frac) +
                           v[static_cast<std::size_t>(k + 1)] * frac;
        table[i] = static_cast<uint8_t>(std::clamp(std::lround(val * 255.0), 0L, 255L));
      }
      break;
    }
    case FuncType::Discrete: {
      const auto& v = func.tableValues;
      const int n = static_cast<int>(v.size());
      if (n == 0) {
        for (int i = 0; i < 256; ++i) {
          table[i] = static_cast<uint8_t>(i);
        }
        break;
      }
      for (int i = 0; i < 256; ++i) {
        const int k = std::min(static_cast<int>(static_cast<double>(i) / 255.0 * n), n - 1);
        table[i] = static_cast<uint8_t>(
            std::clamp(std::lround(v[static_cast<std::size_t>(k)] * 255.0), 0L, 255L));
      }
      break;
    }
    case FuncType::Linear:
      for (int i = 0; i < 256; ++i) {
        const double val = func.slope * static_cast<double>(i) / 255.0 + func.intercept;
        table[i] = static_cast<uint8_t>(std::clamp(std::lround(val * 255.0), 0L, 255L));
      }
      break;
    case FuncType::Gamma:
      for (int i = 0; i < 256; ++i) {
        const double val =
            func.amplitude * std::pow(static_cast<double>(i) / 255.0, func.exponent) + func.offset;
        table[i] = static_cast<uint8_t>(std::clamp(std::lround(val * 255.0), 0L, 255L));
      }
      break;
  }
}

/// Attempt to lower the entire FilterGraph to a single SkImageFilter DAG.
/// Returns nullptr if any node can't be lowered (caller should fall back to CPU path).
sk_sp<SkImageFilter> buildNativeSkiaFilterDAG(const components::FilterGraph& filterGraph,
                                              const Transformd& deviceFromFilter) {
  namespace fp = components::filter_primitive;

  // Track the implicit previous result and named results.
  sk_sp<SkImageFilter> previousFilter;  // nullptr = SourceGraphic
  std::map<RcString, sk_sp<SkImageFilter>> namedResults;

  // SourceAlpha: extract alpha channel (zero RGB, keep A).
  auto makeSourceAlpha = [](sk_sp<SkImageFilter> input) -> sk_sp<SkImageFilter> {
    float alphaMatrix[20] = {};
    alphaMatrix[18] = 1.0f;  // A = A
    return SkImageFilters::ColorFilter(SkColorFilters::Matrix(alphaMatrix), std::move(input));
  };

  // Determine the default color space for the graph.
  const bool graphUsesLinearRGB =
      filterGraph.colorInterpolationFilters == ColorInterpolationFilters::LinearRGB;

  // Resolve a node's subregion to user-space Boxd. Used by feTile (needs input subregion)
  // and feImage (needs viewport for preserveAspectRatio mapping).
  const bool isOBB = filterGraph.primitiveUnits == PrimitiveUnits::ObjectBoundingBox &&
                     filterGraph.elementBoundingBox.has_value();
  const Boxd defaultSubregion = filterGraph.filterRegion.value_or(Boxd::FromXYWH(0, 0, 1, 1));

  auto resolveNodeSubregion = [&](const components::FilterNode& n) -> Boxd {
    if (!n.x.has_value() && !n.y.has_value() && !n.width.has_value() && !n.height.has_value()) {
      return defaultSubregion;
    }
    const Boxd& bbox = isOBB ? *filterGraph.elementBoundingBox : defaultSubregion;
    auto resolvePos = [&](const std::optional<Lengthd>& len, Lengthd::Extent ext, double origin,
                          double bboxDim) -> double {
      if (!len.has_value()) {
        return ext == Lengthd::Extent::X ? defaultSubregion.topLeft.x : defaultSubregion.topLeft.y;
      }
      if (isOBB && len->unit == Lengthd::Unit::None) {
        return origin + len->value * bboxDim;
      }
      if (len->unit == Lengthd::Unit::Percent) {
        const double refSize = ext == Lengthd::Extent::X ? bbox.width() : bbox.height();
        const double refOrigin = ext == Lengthd::Extent::X ? bbox.topLeft.x : bbox.topLeft.y;
        return refOrigin + refSize * len->value / 100.0;
      }
      return len->toPixels(bbox, FontMetrics(), ext);
    };
    auto resolveSize = [&](const std::optional<Lengthd>& len, Lengthd::Extent ext,
                           double bboxDim) -> double {
      if (!len.has_value()) {
        return ext == Lengthd::Extent::X ? defaultSubregion.width() : defaultSubregion.height();
      }
      if (isOBB && len->unit == Lengthd::Unit::None) {
        return len->value * bboxDim;
      }
      if (len->unit == Lengthd::Unit::Percent) {
        const double refSize = ext == Lengthd::Extent::X ? bbox.width() : bbox.height();
        return refSize * len->value / 100.0;
      }
      return len->toPixels(bbox, FontMetrics(), ext);
    };
    const double bboxW = isOBB ? bbox.width() : 1.0;
    const double bboxH = isOBB ? bbox.height() : 1.0;
    const double bboxX = isOBB ? bbox.topLeft.x : 0.0;
    const double bboxY = isOBB ? bbox.topLeft.y : 0.0;
    const double x = resolvePos(n.x, Lengthd::Extent::X, bboxX, bboxW);
    const double y = resolvePos(n.y, Lengthd::Extent::Y, bboxY, bboxH);
    const double w = resolveSize(n.width, Lengthd::Extent::X, bboxW);
    const double h = resolveSize(n.height, Lengthd::Extent::Y, bboxH);
    return Boxd::FromXYWH(x, y, w, h);
  };

  // Track the previous node's resolved subregion (for feTile input subregion).
  Boxd previousSubregion = defaultSubregion;
  std::map<RcString, Boxd> namedSubregions;

  for (const components::FilterNode& node : filterGraph.nodes) {
    // Determine this node's color space.
    const bool nodeUsesLinearRGB =
        node.colorInterpolationFilters.has_value()
            ? (*node.colorInterpolationFilters == ColorInterpolationFilters::LinearRGB)
            : graphUsesLinearRGB;

    // Resolve inputs.
    auto resolveInput = [&](const components::FilterInput& input) -> sk_sp<SkImageFilter> {
      return std::visit(
          [&](const auto& v) -> sk_sp<SkImageFilter> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, components::FilterInput::Previous>) {
              return previousFilter;
            } else if constexpr (std::is_same_v<T, components::FilterStandardInput>) {
              switch (v) {
                case components::FilterStandardInput::SourceGraphic: return nullptr;
                case components::FilterStandardInput::SourceAlpha: return makeSourceAlpha(nullptr);
                case components::FilterStandardInput::FillPaint:
                case components::FilterStandardInput::StrokePaint:
                  // FillPaint/StrokePaint require pre-rendered images that aren't available
                  // in the native Skia filter path (we'd need to pre-render them).
                  // For now, return nullptr (transparent source) as a fallback.
                  // TODO: Support FillPaint/StrokePaint by pre-rendering into SkImage.
                  return nullptr;
              }
              return nullptr;
            } else if constexpr (std::is_same_v<T, components::FilterInput::Named>) {
              auto it = namedResults.find(v.name);
              if (it != namedResults.end()) {
                return it->second;
              }
              return previousFilter;  // Fallback per SVG spec.
            }
          },
          input.value);
    };

    // Resolve first (and possibly second) input.
    sk_sp<SkImageFilter> in1 = node.inputs.empty() ? previousFilter : resolveInput(node.inputs[0]);
    sk_sp<SkImageFilter> in2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : nullptr;

    // Apply color space conversion to inputs if this node uses linearRGB.
    auto applyColorSpace = [&](sk_sp<SkImageFilter> filter) -> sk_sp<SkImageFilter> {
      if (nodeUsesLinearRGB) {
        return SkImageFilters::ColorFilter(SkColorFilters::SRGBToLinearGamma(), std::move(filter));
      }
      return filter;
    };

    in1 = applyColorSpace(std::move(in1));
    if (in2) {
      in2 = applyColorSpace(std::move(in2));
    }

    // Lower the primitive to an SkImageFilter.
    sk_sp<SkImageFilter> result;

    const bool lowered = std::visit(
        [&](const auto& primitive) -> bool {
          using T = std::decay_t<decltype(primitive)>;

          if constexpr (std::is_same_v<T, fp::GaussianBlur>) {
            result =
                SkImageFilters::Blur(static_cast<SkScalar>(primitive.stdDeviationX),
                                     static_cast<SkScalar>(primitive.stdDeviationY),
                                     blurEdgeModeToTileMode(primitive.edgeMode), std::move(in1));
            return true;

          } else if constexpr (std::is_same_v<T, fp::Offset>) {
            result = SkImageFilters::Offset(static_cast<SkScalar>(primitive.dx),
                                            static_cast<SkScalar>(primitive.dy), std::move(in1));
            return true;

          } else if constexpr (std::is_same_v<T, fp::Flood>) {
            const auto rgba = primitive.floodColor.asRGBA();
            const float a = static_cast<float>(primitive.floodOpacity);
            const SkColor4f color = {
                static_cast<float>(rgba.r) / 255.0f * a, static_cast<float>(rgba.g) / 255.0f * a,
                static_cast<float>(rgba.b) / 255.0f * a, static_cast<float>(rgba.a) / 255.0f * a};
            result = SkImageFilters::Shader(SkShaders::Color(color, nullptr));
            return true;

          } else if constexpr (std::is_same_v<T, fp::Blend>) {
            result = SkImageFilters::Blend(toSkiaBlendMode(primitive.mode), std::move(in1),
                                           std::move(in2));
            return true;

          } else if constexpr (std::is_same_v<T, fp::Composite>) {
            if (primitive.op == fp::Composite::Operator::Arithmetic) {
              result = SkImageFilters::Arithmetic(
                  static_cast<SkScalar>(primitive.k1), static_cast<SkScalar>(primitive.k2),
                  static_cast<SkScalar>(primitive.k3), static_cast<SkScalar>(primitive.k4),
                  true /* enforcePMColor */, std::move(in1), std::move(in2));
            } else {
              auto blendMode = compositeOpToBlendMode(primitive.op);
              if (!blendMode) {
                return false;
              }
              result = SkImageFilters::Blend(*blendMode, std::move(in1), std::move(in2));
            }
            return true;

          } else if constexpr (std::is_same_v<T, fp::Merge>) {
            // Collect all inputs for merge.
            std::vector<sk_sp<SkImageFilter>> mergeInputs;
            mergeInputs.reserve(node.inputs.size());
            // Re-resolve all inputs (in1/in2 only covers first two).
            for (const auto& input : node.inputs) {
              mergeInputs.push_back(applyColorSpace(resolveInput(input)));
            }
            result =
                SkImageFilters::Merge(mergeInputs.data(), static_cast<int>(mergeInputs.size()));
            return true;

          } else if constexpr (std::is_same_v<T, fp::ColorMatrix>) {
            const auto matrix = buildColorMatrix(primitive);
            result =
                SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix.data()), std::move(in1));
            return true;

          } else if constexpr (std::is_same_v<T, fp::DropShadow>) {
            const auto rgba = primitive.floodColor.asRGBA();
            const float opacity = static_cast<float>(primitive.floodOpacity);
            result = SkImageFilters::DropShadow(
                static_cast<SkScalar>(primitive.dx), static_cast<SkScalar>(primitive.dy),
                static_cast<SkScalar>(primitive.stdDeviationX),
                static_cast<SkScalar>(primitive.stdDeviationY),
                SkColorSetARGB(static_cast<uint8_t>(std::clamp(rgba.a * opacity, 0.0f, 255.0f)),
                               rgba.r, rgba.g, rgba.b),
                std::move(in1));
            return true;

          } else if constexpr (std::is_same_v<T, fp::Morphology>) {
            if (primitive.op == fp::Morphology::Operator::Dilate) {
              result =
                  SkImageFilters::Dilate(static_cast<SkScalar>(primitive.radiusX),
                                         static_cast<SkScalar>(primitive.radiusY), std::move(in1));
            } else {
              result =
                  SkImageFilters::Erode(static_cast<SkScalar>(primitive.radiusX),
                                        static_cast<SkScalar>(primitive.radiusY), std::move(in1));
            }
            return true;

          } else if constexpr (std::is_same_v<T, fp::ComponentTransfer>) {
            uint8_t tableR[256], tableG[256], tableB[256], tableA[256];
            buildTransferTable(primitive.funcR, tableR);
            buildTransferTable(primitive.funcG, tableG);
            buildTransferTable(primitive.funcB, tableB);
            buildTransferTable(primitive.funcA, tableA);
            result = SkImageFilters::ColorFilter(
                SkColorFilters::TableARGB(tableA, tableR, tableG, tableB), std::move(in1));
            return true;

          } else if constexpr (std::is_same_v<T, fp::ConvolveMatrix>) {
            const int orderX = primitive.orderX;
            const int orderY = primitive.orderY;
            const int targetX = primitive.targetX.value_or(orderX / 2);
            const int targetY = primitive.targetY.value_or(orderY / 2);
            double divisor = primitive.divisor.value_or(0.0);
            if (divisor == 0.0) {
              divisor = 0.0;
              for (double v : primitive.kernelMatrix) {
                divisor += v;
              }
              if (NearZero(divisor)) {
                divisor = 1.0;
              }
            }
            const float gain = static_cast<float>(1.0 / divisor);
            const float bias = static_cast<float>(primitive.bias);

            std::vector<SkScalar> kernel(primitive.kernelMatrix.size());
            for (std::size_t i = 0; i < primitive.kernelMatrix.size(); ++i) {
              kernel[i] = static_cast<SkScalar>(primitive.kernelMatrix[i]);
            }

            result = SkImageFilters::MatrixConvolution(
                SkISize::Make(orderX, orderY), kernel.data(), gain, bias,
                SkIPoint::Make(targetX, targetY), convolveEdgeModeToTileMode(primitive.edgeMode),
                !primitive.preserveAlpha, std::move(in1));
            return true;

          } else if constexpr (std::is_same_v<T, fp::DisplacementMap>) {
            // Note: Skia's DisplacementMap takes (displacement, color) — in2 is displacement.
            result = SkImageFilters::DisplacementMap(toSkiaColorChannel(primitive.xChannelSelector),
                                                     toSkiaColorChannel(primitive.yChannelSelector),
                                                     static_cast<SkScalar>(primitive.scale),
                                                     std::move(in2), std::move(in1));
            return true;

          } else if constexpr (std::is_same_v<T, fp::Turbulence>) {
            sk_sp<SkShader> shader;
            if (primitive.type == fp::Turbulence::Type::FractalNoise) {
              shader = SkShaders::MakeFractalNoise(static_cast<SkScalar>(primitive.baseFrequencyX),
                                                   static_cast<SkScalar>(primitive.baseFrequencyY),
                                                   primitive.numOctaves,
                                                   static_cast<SkScalar>(primitive.seed));
            } else {
              shader = SkShaders::MakeTurbulence(static_cast<SkScalar>(primitive.baseFrequencyX),
                                                 static_cast<SkScalar>(primitive.baseFrequencyY),
                                                 primitive.numOctaves,
                                                 static_cast<SkScalar>(primitive.seed));
            }
            result = SkImageFilters::Shader(std::move(shader));
            return true;

          } else if constexpr (std::is_same_v<T, fp::DiffuseLighting>) {
            if (!primitive.light.has_value()) {
              return false;
            }
            const auto& light = *primitive.light;
            const auto lightRgba = primitive.lightingColor.asRGBA();
            const SkColor lightColor = toSkia(lightRgba);
            const SkScalar surfaceScale = static_cast<SkScalar>(primitive.surfaceScale);
            const SkScalar kd = static_cast<SkScalar>(primitive.diffuseConstant);

            switch (light.type) {
              case fp::LightSource::Type::Distant: {
                const double azRad = light.azimuth * std::numbers::pi / 180.0;
                const double elRad = light.elevation * std::numbers::pi / 180.0;
                const SkPoint3 dir = {static_cast<SkScalar>(std::cos(azRad) * std::cos(elRad)),
                                      static_cast<SkScalar>(std::sin(azRad) * std::cos(elRad)),
                                      static_cast<SkScalar>(std::sin(elRad))};
                result = SkImageFilters::DistantLitDiffuse(dir, lightColor, surfaceScale, kd,
                                                           std::move(in1));
                break;
              }
              case fp::LightSource::Type::Point: {
                const SkPoint3 location = {static_cast<SkScalar>(light.x),
                                           static_cast<SkScalar>(light.y),
                                           static_cast<SkScalar>(light.z)};
                result = SkImageFilters::PointLitDiffuse(location, lightColor, surfaceScale, kd,
                                                         std::move(in1));
                break;
              }
              case fp::LightSource::Type::Spot: {
                const SkPoint3 location = {static_cast<SkScalar>(light.x),
                                           static_cast<SkScalar>(light.y),
                                           static_cast<SkScalar>(light.z)};
                const SkPoint3 target = {static_cast<SkScalar>(light.pointsAtX),
                                         static_cast<SkScalar>(light.pointsAtY),
                                         static_cast<SkScalar>(light.pointsAtZ)};
                const SkScalar cutoff = light.limitingConeAngle.has_value()
                                            ? static_cast<SkScalar>(*light.limitingConeAngle)
                                            : 180.0f;
                result = SkImageFilters::SpotLitDiffuse(
                    location, target, static_cast<SkScalar>(light.spotExponent), cutoff, lightColor,
                    surfaceScale, kd, std::move(in1));
                break;
              }
            }
            return true;

          } else if constexpr (std::is_same_v<T, fp::SpecularLighting>) {
            if (!primitive.light.has_value()) {
              return false;
            }
            const auto& light = *primitive.light;
            const auto lightRgba = primitive.lightingColor.asRGBA();
            const SkColor lightColor = toSkia(lightRgba);
            const SkScalar surfaceScale = static_cast<SkScalar>(primitive.surfaceScale);
            const SkScalar ks = static_cast<SkScalar>(primitive.specularConstant);
            const SkScalar shininess = static_cast<SkScalar>(primitive.specularExponent);

            switch (light.type) {
              case fp::LightSource::Type::Distant: {
                const double azRad = light.azimuth * std::numbers::pi / 180.0;
                const double elRad = light.elevation * std::numbers::pi / 180.0;
                const SkPoint3 dir = {static_cast<SkScalar>(std::cos(azRad) * std::cos(elRad)),
                                      static_cast<SkScalar>(std::sin(azRad) * std::cos(elRad)),
                                      static_cast<SkScalar>(std::sin(elRad))};
                result = SkImageFilters::DistantLitSpecular(dir, lightColor, surfaceScale, ks,
                                                            shininess, std::move(in1));
                break;
              }
              case fp::LightSource::Type::Point: {
                const SkPoint3 location = {static_cast<SkScalar>(light.x),
                                           static_cast<SkScalar>(light.y),
                                           static_cast<SkScalar>(light.z)};
                result = SkImageFilters::PointLitSpecular(location, lightColor, surfaceScale, ks,
                                                          shininess, std::move(in1));
                break;
              }
              case fp::LightSource::Type::Spot: {
                const SkPoint3 location = {static_cast<SkScalar>(light.x),
                                           static_cast<SkScalar>(light.y),
                                           static_cast<SkScalar>(light.z)};
                const SkPoint3 target = {static_cast<SkScalar>(light.pointsAtX),
                                         static_cast<SkScalar>(light.pointsAtY),
                                         static_cast<SkScalar>(light.pointsAtZ)};
                const SkScalar cutoff = light.limitingConeAngle.has_value()
                                            ? static_cast<SkScalar>(*light.limitingConeAngle)
                                            : 180.0f;
                result = SkImageFilters::SpotLitSpecular(
                    location, target, static_cast<SkScalar>(light.spotExponent), cutoff, lightColor,
                    surfaceScale, ks, shininess, std::move(in1));
                break;
              }
            }
            return true;

          } else if constexpr (std::is_same_v<T, fp::Tile>) {
            // feTile tiles the input's subregion to fill the output.
            // Source rect = input node's subregion, dest rect = filter region.
            Boxd inputSubregion = previousSubregion;
            if (!node.inputs.empty()) {
              const auto& input = node.inputs[0];
              std::visit(
                  [&](const auto& v) {
                    using V = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<V, components::FilterInput::Named>) {
                      auto it = namedSubregions.find(v.name);
                      if (it != namedSubregions.end()) {
                        inputSubregion = it->second;
                      }
                    }
                  },
                  input.value);
            }
            const SkRect srcRect = SkRect::MakeXYWH(static_cast<SkScalar>(inputSubregion.topLeft.x),
                                                    static_cast<SkScalar>(inputSubregion.topLeft.y),
                                                    static_cast<SkScalar>(inputSubregion.width()),
                                                    static_cast<SkScalar>(inputSubregion.height()));
            const Boxd dstRegion = resolveNodeSubregion(node);
            const SkRect dstRect = SkRect::MakeXYWH(static_cast<SkScalar>(dstRegion.topLeft.x),
                                                    static_cast<SkScalar>(dstRegion.topLeft.y),
                                                    static_cast<SkScalar>(dstRegion.width()),
                                                    static_cast<SkScalar>(dstRegion.height()));
            result = SkImageFilters::Tile(srcRect, dstRect, std::move(in1));
            return true;

          } else if constexpr (std::is_same_v<T, fp::Image>) {
            if (primitive.imageData.empty() || primitive.imageWidth <= 0 ||
                primitive.imageHeight <= 0) {
              // No image data — produce transparent output.
              result = SkImageFilters::Shader(SkShaders::Color(SkColor4f{0, 0, 0, 0}, nullptr));
              return true;
            }

            // Premultiply the image data and create an SkImage.
            const auto premultiplied = PremultiplyRgba(primitive.imageData);
            const SkImageInfo imageInfo =
                SkImageInfo::Make(primitive.imageWidth, primitive.imageHeight,
                                  kRGBA_8888_SkColorType, kPremul_SkAlphaType);
            const SkPixmap pixmap(imageInfo, premultiplied.data(),
                                  static_cast<size_t>(primitive.imageWidth) * 4);
            sk_sp<SkImage> skImage = SkImages::RasterFromPixmapCopy(pixmap);
            if (!skImage) {
              return false;
            }

            const SkRect srcRect = SkRect::MakeWH(static_cast<SkScalar>(primitive.imageWidth),
                                                  static_cast<SkScalar>(primitive.imageHeight));

            if (primitive.isFragmentReference) {
              // Fragment references: place at (0,0) with 1:1 mapping.
              result = SkImageFilters::Image(
                  std::move(skImage),
                  SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNearest));
              return true;
            }

            // Compute viewport from node's subregion for preserveAspectRatio mapping.
            const Boxd viewport = resolveNodeSubregion(node);
            const Boxd imageBox = Boxd::FromXYWH(0, 0, primitive.imageWidth, primitive.imageHeight);
            const Boxd viewportLocal = Boxd::FromXYWH(0, 0, viewport.width(), viewport.height());
            const Transformd viewportFromImage =
                primitive.preserveAspectRatio.elementContentFromViewBoxTransform(viewportLocal,
                                                                                 imageBox);

            const Vector2d topLeft = viewportFromImage.transformPosition(Vector2d(0, 0));
            const Vector2d bottomRight = viewportFromImage.transformPosition(
                Vector2d(primitive.imageWidth, primitive.imageHeight));
            const SkRect dstRect = SkRect::MakeXYWH(
                static_cast<SkScalar>(std::min(topLeft.x, bottomRight.x) + viewport.topLeft.x),
                static_cast<SkScalar>(std::min(topLeft.y, bottomRight.y) + viewport.topLeft.y),
                static_cast<SkScalar>(std::abs(bottomRight.x - topLeft.x)),
                static_cast<SkScalar>(std::abs(bottomRight.y - topLeft.y)));

            result = SkImageFilters::Image(
                std::move(skImage), srcRect, dstRect,
                SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNearest));
            return true;

          } else {
            return false;  // Unknown primitive type.
          }
        },
        node.primitive);

    if (!lowered || !result) {
      return nullptr;  // Can't lower this node — fall back to CPU.
    }

    // Convert back from linearRGB to sRGB if the node operated in linear.
    if (nodeUsesLinearRGB) {
      result = SkImageFilters::ColorFilter(SkColorFilters::LinearToSRGBGamma(), std::move(result));
    }

    // Store named result if this node has a result attribute.
    const Boxd nodeSubregion = resolveNodeSubregion(node);
    if (node.result.has_value()) {
      namedResults[*node.result] = result;
      namedSubregions[*node.result] = nodeSubregion;
    }
    previousFilter = std::move(result);
    previousSubregion = nodeSubregion;
  }

  return previousFilter;
}

Vector2d patternRasterScaleForTransform(const Transformd& deviceFromPattern) {
  const double scaleX =
      std::max(1.0, deviceFromPattern.transformVector(Vector2d(1.0, 0.0)).length());
  const double scaleY =
      std::max(1.0, deviceFromPattern.transformVector(Vector2d(0.0, 1.0)).length());
  constexpr double kPatternSupersampleScale = 2.0;
  return Vector2d(scaleX * kPatternSupersampleScale, scaleY * kPatternSupersampleScale);
}

Vector2d effectivePatternRasterScale(const Boxd& tileRect, int pixelWidth, int pixelHeight,
                                     const Vector2d& fallbackScale) {
  const double scaleX = NearZero(tileRect.width())
                            ? fallbackScale.x
                            : static_cast<double>(pixelWidth) / tileRect.width();
  const double scaleY = NearZero(tileRect.height())
                            ? fallbackScale.y
                            : static_cast<double>(pixelHeight) / tileRect.height();
  return Vector2d(scaleX, scaleY);
}

Transformd scaleTransformOutput(const Transformd& transform, const Vector2d& scale) {
  Transformd result = transform;
  result.data[0] *= scale.x;
  result.data[2] *= scale.x;
  result.data[4] *= scale.x;
  result.data[1] *= scale.y;
  result.data[3] *= scale.y;
  result.data[5] *= scale.y;
  return result;
}

inline Lengthd toPercent(Lengthd value, bool numbersArePercent) {
  if (!numbersArePercent) {
    return value;
  }

  if (value.unit == Lengthd::Unit::None) {
    value.value *= 100.0;
    value.unit = Lengthd::Unit::Percent;
  }

  return value;
}

inline SkScalar resolveGradientCoord(Lengthd value, const Boxd& viewBox, bool numbersArePercent) {
  return NarrowToFloat(toPercent(value, numbersArePercent).toPixels(viewBox, FontMetrics()));
}

Vector2d resolveGradientCoords(Lengthd x, Lengthd y, const Boxd& viewBox, bool numbersArePercent) {
  return Vector2d(
      toPercent(x, numbersArePercent).toPixels(viewBox, FontMetrics(), Lengthd::Extent::X),
      toPercent(y, numbersArePercent).toPixels(viewBox, FontMetrics(), Lengthd::Extent::Y));
}

Transformd resolveGradientTransform(
    const components::ComputedLocalTransformComponent* maybeTransformComponent,
    const Boxd& viewBox) {
  if (maybeTransformComponent == nullptr) {
    return Transformd();
  }

  const Vector2d origin = maybeTransformComponent->transformOrigin;
  const Transformd entityFromParent =
      maybeTransformComponent->rawCssTransform.compute(viewBox, FontMetrics());
  return Transformd::Translate(origin) * entityFromParent * Transformd::Translate(-origin);
}

std::optional<SkPaint> instantiateGradientPaint(const components::PaintResolvedReference& ref,
                                                const Boxd& pathBounds, const Boxd& viewBox,
                                                const css::RGBA currentColor, float opacity,
                                                bool antialias) {
  const EntityHandle handle = ref.reference.handle;
  if (!handle) {
    return std::nullopt;
  }

  const auto* computedGradient = handle.try_get<components::ComputedGradientComponent>();
  if (computedGradient == nullptr || !computedGradient->initialized) {
    return std::nullopt;
  }

  const bool objectBoundingBox =
      computedGradient->gradientUnits == GradientUnits::ObjectBoundingBox;
  const bool numbersArePercent = objectBoundingBox;

  // Use a generous tolerance for degenerate bounding box detection: cubic bezier computation
  // can produce floating-point artifacts (e.g. 1.4e-14 width for a perfectly vertical path).
  constexpr double kDegenerateBBoxTolerance = 1e-6;
  if (objectBoundingBox && (NearZero(pathBounds.width(), kDegenerateBBoxTolerance) ||
                            NearZero(pathBounds.height(), kDegenerateBBoxTolerance))) {
    return std::nullopt;
  }

  Transformd gradientFromGradientUnits;
  if (objectBoundingBox) {
    gradientFromGradientUnits = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), kUnitPathBounds);

    const Transformd objectBoundingBoxFromUnitBox =
        Transformd::Scale(pathBounds.size()) * Transformd::Translate(pathBounds.topLeft);
    gradientFromGradientUnits = gradientFromGradientUnits * objectBoundingBoxFromUnitBox;
  } else {
    gradientFromGradientUnits = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), viewBox);
  }

  const Boxd& bounds = objectBoundingBox ? kUnitPathBounds : viewBox;

  std::vector<SkScalar> positions;
  std::vector<SkColor> colors;
  positions.reserve(computedGradient->stops.size());
  colors.reserve(computedGradient->stops.size());
  for (const GradientStop& stop : computedGradient->stops) {
    positions.push_back(stop.offset);
    colors.push_back(toSkia(stop.color.resolve(currentColor, stop.opacity * opacity)));
  }

  if (positions.empty()) {
    return std::nullopt;
  }

  if (positions.size() == 1) {
    SkPaint paint;
    paint.setAntiAlias(antialias);
    paint.setColor(colors[0]);
    return paint;
  }

  const SkMatrix skGradientFromGradientUnits = toSkiaMatrix(gradientFromGradientUnits);

  SkPaint paint;
  paint.setAntiAlias(antialias);

  if (const auto* linear = handle.try_get<components::ComputedLinearGradientComponent>()) {
    const Vector2d start = resolveGradientCoords(linear->x1, linear->y1, bounds, numbersArePercent);
    const Vector2d end = resolveGradientCoords(linear->x2, linear->y2, bounds, numbersArePercent);

    const SkPoint points[] = {toSkia(start), toSkia(end)};
    paint.setShader(SkGradientShader::MakeLinear(
        points, colors.data(), positions.data(), static_cast<int>(positions.size()),
        toSkia(computedGradient->spreadMethod), 0, &skGradientFromGradientUnits));
    return paint;
  }

  if (const auto* radial = handle.try_get<components::ComputedRadialGradientComponent>()) {
    const double radius = resolveGradientCoord(radial->r, bounds, numbersArePercent);
    const Vector2d center =
        resolveGradientCoords(radial->cx, radial->cy, bounds, numbersArePercent);
    const double focalRadius = resolveGradientCoord(radial->fr, bounds, numbersArePercent);
    const Vector2d focalCenter =
        resolveGradientCoords(radial->fx.value_or(radial->cx), radial->fy.value_or(radial->cy),
                              bounds, numbersArePercent);

    if (NearZero(radius)) {
      SkPaint solidPaint;
      solidPaint.setAntiAlias(antialias);
      solidPaint.setColor(colors.back());
      return solidPaint;
    }

    // SVG2: If the start (focal) circle fully overlaps the end circle, no gradient is drawn.
    // https://www.w3.org/TR/SVG2/pservers.html#RadialGradientNotes
    const double distanceBetweenCenters = (center - focalCenter).length();
    if (distanceBetweenCenters + radius <= focalRadius) {
      return std::nullopt;
    }

    const float skRadius = static_cast<float>(radius);
    if (NearZero(focalRadius) && focalCenter == center) {
      paint.setShader(SkGradientShader::MakeRadial(
          toSkia(center), skRadius, colors.data(), positions.data(),
          static_cast<int>(positions.size()), toSkia(computedGradient->spreadMethod), 0,
          &skGradientFromGradientUnits));
    } else {
      paint.setShader(SkGradientShader::MakeTwoPointConical(
          toSkia(focalCenter), static_cast<SkScalar>(focalRadius), toSkia(center), skRadius,
          colors.data(), positions.data(), static_cast<int>(positions.size()),
          toSkia(computedGradient->spreadMethod), 0, &skGradientFromGradientUnits));
    }
    return paint;
  }

  return std::nullopt;
}

SkPaint basePaint(bool antialias, double opacity) {
  SkPaint paint;
  paint.setAntiAlias(antialias);
  paint.setAlphaf(NarrowToFloat(opacity));
  return paint;
}

SkPaint::Cap toSkia(StrokeLinecap lineCap) {
  switch (lineCap) {
    case StrokeLinecap::Butt: return SkPaint::Cap::kButt_Cap;
    case StrokeLinecap::Round: return SkPaint::Cap::kRound_Cap;
    case StrokeLinecap::Square: return SkPaint::Cap::kSquare_Cap;
  }

  UTILS_UNREACHABLE();
}

SkPaint::Join toSkia(StrokeLinejoin lineJoin) {
  // TODO(jwmcglynn): Implement MiterClip and Arcs. For now, fallback to Miter, which is the default
  // linejoin, since the feature is not implemented.
  switch (lineJoin) {
    case StrokeLinejoin::Miter: return SkPaint::Join::kMiter_Join;
    case StrokeLinejoin::MiterClip: return SkPaint::Join::kMiter_Join;
    case StrokeLinejoin::Round: return SkPaint::Join::kRound_Join;
    case StrokeLinejoin::Bevel: return SkPaint::Join::kBevel_Join;
    case StrokeLinejoin::Arcs: return SkPaint::Join::kMiter_Join;
  }

  UTILS_UNREACHABLE();
}

SkPath toSkia(const PathSpline& spline) {
  SkPath path;

  const std::vector<Vector2d>& points = spline.points();
  for (const PathSpline::Command& command : spline.commands()) {
    switch (command.type) {
      case PathSpline::CommandType::MoveTo: {
        auto pt = points[command.pointIndex];
        path.moveTo(static_cast<SkScalar>(pt.x), static_cast<SkScalar>(pt.y));
        break;
      }
      case PathSpline::CommandType::CurveTo: {
        auto c0 = points[command.pointIndex];
        auto c1 = points[command.pointIndex + 1];
        auto end = points[command.pointIndex + 2];
        path.cubicTo(static_cast<SkScalar>(c0.x), static_cast<SkScalar>(c0.y),
                     static_cast<SkScalar>(c1.x), static_cast<SkScalar>(c1.y),
                     static_cast<SkScalar>(end.x), static_cast<SkScalar>(end.y));
        break;
      }
      case PathSpline::CommandType::LineTo: {
        auto pt = points[command.pointIndex];
        path.lineTo(static_cast<SkScalar>(pt.x), static_cast<SkScalar>(pt.y));
        break;
      }
      case PathSpline::CommandType::ClosePath: {
        path.close();
        break;
      }
    }
  }

  return path;
}

SkTileMode toSkia(GradientSpreadMethod spreadMethod) {
  switch (spreadMethod) {
    case GradientSpreadMethod::Pad: return SkTileMode::kClamp;
    case GradientSpreadMethod::Reflect: return SkTileMode::kMirror;
    case GradientSpreadMethod::Repeat: return SkTileMode::kRepeat;
  }

  UTILS_UNREACHABLE();
}

}  // namespace

RendererSkia::RendererSkia(bool verbose) : verbose_(verbose) {
#if defined(DONNER_USE_CORETEXT)
  fontMgr_ = SkFontMgr_New_CoreText(nullptr);
#elif defined(DONNER_USE_FREETYPE_WITH_FONTCONFIG)
  fontMgr_ = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#elif defined(DONNER_USE_FREETYPE)
  fontMgr_ = SkFontMgr_New_Custom_Empty();
#endif
}

RendererSkia::~RendererSkia() {}

RendererSkia::RendererSkia(RendererSkia&&) noexcept = default;
RendererSkia& RendererSkia::operator=(RendererSkia&&) noexcept = default;

void RendererSkia::beginFrame(const RenderViewport& viewport) {
  viewport_ = viewport;
  const int pixelWidth = static_cast<int>(viewport.size.x * viewport.devicePixelRatio);
  const int pixelHeight = static_cast<int>(viewport.size.y * viewport.devicePixelRatio);

  bitmap_.allocPixels(
      SkImageInfo::MakeN32(pixelWidth, pixelHeight, SkAlphaType::kUnpremul_SkAlphaType));
  bitmap_.eraseColor(SK_ColorTRANSPARENT);

  if (externalCanvas_ != nullptr) {
    currentCanvas_ = externalCanvas_;
  } else {
    bitmapCanvas_ = std::make_unique<SkCanvas>(bitmap_);
    currentCanvas_ = bitmapCanvas_.get();
  }

  transformDepth_ = 0;
  clipDepth_ = 0;
  activeClips_.clear();
  filterLayerStack_.clear();
}

void RendererSkia::endFrame() {
  for (; clipDepth_ > 0; --clipDepth_) {
    currentCanvas_->restore();
  }
  for (; transformDepth_ > 0; --transformDepth_) {
    currentCanvas_->restore();
  }

  currentCanvas_ = nullptr;
  externalCanvas_ = nullptr;
  bitmapCanvas_.reset();
  activeClips_.clear();
  filterLayerStack_.clear();
}

void RendererSkia::setTransform(const Transformd& transform) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  currentCanvas_->setMatrix(toSkiaMatrix(transform));
}

void RendererSkia::pushTransform(const Transformd& transform) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  currentCanvas_->save();
  currentCanvas_->concat(toSkiaMatrix(transform));
  ++transformDepth_;
}

void RendererSkia::popTransform() {
  if (currentCanvas_ == nullptr || transformDepth_ <= 0) {
    return;
  }

  currentCanvas_->restore();
  --transformDepth_;
}

void RendererSkia::pushClip(const ResolvedClip& clip) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  const SkMatrix clipMatrix = currentCanvas_->getTotalMatrix();
  currentCanvas_->save();
  applyClipToCanvas(currentCanvas_, clip);
  ++clipDepth_;
  activeClips_.push_back(ClipStackEntry{clip, clipMatrix});
}

void RendererSkia::popClip() {
  if (currentCanvas_ == nullptr || clipDepth_ <= 0) {
    return;
  }

  currentCanvas_->restore();
  --clipDepth_;
  if (!activeClips_.empty()) {
    activeClips_.pop_back();
  }
}

std::optional<SkPaint> RendererSkia::makeFillPaint(const Boxd& bounds) {
  if (std::holds_alternative<PaintServer::None>(paint_.fill)) {
    return std::nullopt;
  }

  const css::RGBA currentColor = paint_.currentColor.rgba();
  const float fillOpacity = NarrowToFloat(paint_.fillOpacity);

  // Use pre-recorded pattern tile if available.
  if (patternFillPaint_.has_value()) {
    SkPaint paint = std::move(*patternFillPaint_);
    patternFillPaint_.reset();
    paint.setStyle(SkPaint::Style::kFill_Style);
    paint.setAlphaf(fillOpacity);
    return paint;
  }

  if (const auto* solid = std::get_if<PaintServer::Solid>(&paint_.fill)) {
    SkPaint paint;
    paint.setAntiAlias(antialias_);
    paint.setStyle(SkPaint::Style::kFill_Style);
    paint.setColor(toSkia(solid->color.resolve(currentColor, fillOpacity)));
    return paint;
  }

  if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint_.fill)) {
    if (std::optional<SkPaint> gradient = instantiateGradientPaint(
            *ref, bounds, paint_.viewBox, currentColor, fillOpacity, antialias_)) {
      gradient->setStyle(SkPaint::Style::kFill_Style);
      return gradient;
    }

    if (ref->fallback) {
      SkPaint paint;
      paint.setAntiAlias(antialias_);
      paint.setStyle(SkPaint::Style::kFill_Style);
      paint.setColor(toSkia(ref->fallback->resolve(currentColor, fillOpacity)));
      return paint;
    }
  }

  return std::nullopt;
}

std::optional<SkPaint> RendererSkia::makeStrokePaint(const Boxd& bounds,
                                                     const StrokeParams& stroke) {
  if (std::holds_alternative<PaintServer::None>(paint_.stroke) || stroke.strokeWidth <= 0.0) {
    return std::nullopt;
  }

  auto configureStroke = [&](SkPaint& paint) {
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(static_cast<SkScalar>(stroke.strokeWidth));
    paint.setStrokeCap(toSkia(stroke.lineCap));
    paint.setStrokeJoin(toSkia(stroke.lineJoin));
    paint.setStrokeMiter(static_cast<SkScalar>(stroke.miterLimit));

    if (!stroke.dashArray.empty()) {
      // Skia requires an even number of dash lengths; repeat odd-length arrays.
      const int numRepeats = (stroke.dashArray.size() & 1) ? 2 : 1;
      std::vector<SkScalar> dashes;
      dashes.reserve(stroke.dashArray.size() * numRepeats);
      for (int i = 0; i < numRepeats; ++i) {
        for (double dash : stroke.dashArray) {
          dashes.push_back(static_cast<SkScalar>(dash));
        }
      }

      paint.setPathEffect(SkDashPathEffect::Make(dashes.data(), static_cast<int>(dashes.size()),
                                                 static_cast<SkScalar>(stroke.dashOffset)));
    }
  };

  const css::RGBA currentColor = paint_.currentColor.rgba();
  const float strokeOpacity = NarrowToFloat(paint_.strokeOpacity);

  // Use pre-recorded pattern tile if available.
  if (patternStrokePaint_.has_value()) {
    SkPaint paint = std::move(*patternStrokePaint_);
    patternStrokePaint_.reset();
    configureStroke(paint);
    paint.setAlphaf(strokeOpacity);
    return paint;
  }

  if (const auto* solid = std::get_if<PaintServer::Solid>(&paint_.stroke)) {
    SkPaint paint;
    paint.setAntiAlias(antialias_);
    configureStroke(paint);
    paint.setColor(toSkia(solid->color.resolve(currentColor, strokeOpacity)));
    return paint;
  }

  if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint_.stroke)) {
    if (std::optional<SkPaint> gradient = instantiateGradientPaint(
            *ref, bounds, paint_.viewBox, currentColor, strokeOpacity, antialias_)) {
      configureStroke(*gradient);
      return gradient;
    }

    if (ref->fallback) {
      SkPaint paint;
      paint.setAntiAlias(antialias_);
      configureStroke(paint);
      paint.setColor(toSkia(ref->fallback->resolve(currentColor, strokeOpacity)));
      return paint;
    }
  }

  return std::nullopt;
}

RendererSkia::FilterLayerState* RendererSkia::currentFilterLayerState() {
  if (filterLayerStack_.empty()) {
    return nullptr;
  }

  FilterLayerState& state = filterLayerStack_.back();
  return state.usesNativeSkiaFilter ? nullptr : &state;
}

void RendererSkia::drawOnFilterInputSurface(const sk_sp<SkSurface>& surface,
                                            const std::function<void(SkCanvas*)>& drawFn) {
  if (surface == nullptr || currentCanvas_ == nullptr) {
    return;
  }

  SkCanvas* canvas = surface->getCanvas();
  canvas->save();
  for (const ClipStackEntry& entry : activeClips_) {
    canvas->save();
    canvas->setMatrix(entry.matrix);
    applyClipToCanvas(canvas, entry.clip);
  }

  canvas->setMatrix(currentCanvas_->getTotalMatrix());
  drawFn(canvas);

  for (std::size_t i = 0; i < activeClips_.size(); ++i) {
    canvas->restore();
  }
  canvas->restore();
}

void RendererSkia::pushFilterLayer(const components::FilterGraph& filterGraph,
                                   const std::optional<Boxd>& filterRegion) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  const Transformd deviceFromFilter = toDonnerTransform(currentCanvas_->getTotalMatrix());

  // Try to lower the entire filter graph to native Skia SkImageFilter DAG.
  if (sk_sp<SkImageFilter> nativeFilter = buildNativeSkiaFilterDAG(filterGraph, deviceFromFilter)) {
    SkPaint filterPaint;
    filterPaint.setAntiAlias(antialias_);
    if (filterRegion.has_value()) {
      const SkRect cropRect = currentCanvas_->getTotalMatrix().mapRect(toSkia(*filterRegion));
      nativeFilter = SkImageFilters::Crop(cropRect, std::move(nativeFilter));
    }
    filterPaint.setImageFilter(std::move(nativeFilter));
    currentCanvas_->saveLayer(nullptr, &filterPaint);

    FilterLayerState state;
    state.usesNativeSkiaFilter = true;
    filterLayerStack_.push_back(std::move(state));
    return;
  }

  const int width = static_cast<int>(viewport_.size.x * viewport_.devicePixelRatio);
  const int height = static_cast<int>(viewport_.size.y * viewport_.devicePixelRatio);
  const SkImageInfo surfaceInfo =
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(surfaceInfo);
  if (surface == nullptr) {
    return;
  }

  auto createFilterInputSurface = [&](bool needed) -> sk_sp<SkSurface> {
    if (!needed) {
      return nullptr;
    }

    sk_sp<SkSurface> inputSurface = SkSurfaces::Raster(surfaceInfo);
    if (inputSurface != nullptr) {
      inputSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
    }
    return inputSurface;
  };

  SkCanvas* filterCanvas = surface->getCanvas();
  filterCanvas->clear(SK_ColorTRANSPARENT);
  for (const ClipStackEntry& entry : activeClips_) {
    filterCanvas->save();
    filterCanvas->setMatrix(entry.matrix);
    applyClipToCanvas(filterCanvas, entry.clip);
  }
  filterCanvas->setMatrix(currentCanvas_->getTotalMatrix());

  FilterLayerState state;
  state.surface = std::move(surface);
  state.fillPaintSurface = createFilterInputSurface(
      graphUsesStandardInput(filterGraph, components::FilterStandardInput::FillPaint));
  state.strokePaintSurface = createFilterInputSurface(
      graphUsesStandardInput(filterGraph, components::FilterStandardInput::StrokePaint));
  state.parentCanvas = currentCanvas_;
  state.filterGraph = filterGraph;
  state.filterRegion = filterRegion;
  state.deviceFromFilter = deviceFromFilter;
  currentCanvas_ = filterCanvas;
  filterLayerStack_.push_back(std::move(state));
}

void RendererSkia::popFilterLayer() {
  if (currentCanvas_ == nullptr || filterLayerStack_.empty()) {
    return;
  }

  FilterLayerState state = std::move(filterLayerStack_.back());
  filterLayerStack_.pop_back();

  if (state.usesNativeSkiaFilter) {
    currentCanvas_->restore();
    return;
  }

  // All SVG filter primitives are lowered to native Skia SkImageFilter in
  // buildNativeSkiaFilterDAG(). If we reach here, the lowering failed for an
  // unexpected reason — just restore the canvas and discard the filter output.
  currentCanvas_ = state.parentCanvas;
}

namespace {

SkBlendMode toSkBlendMode(MixBlendMode mode) {
  switch (mode) {
    case MixBlendMode::Normal: return SkBlendMode::kSrcOver;
    case MixBlendMode::Multiply: return SkBlendMode::kMultiply;
    case MixBlendMode::Screen: return SkBlendMode::kScreen;
    case MixBlendMode::Overlay: return SkBlendMode::kOverlay;
    case MixBlendMode::Darken: return SkBlendMode::kDarken;
    case MixBlendMode::Lighten: return SkBlendMode::kLighten;
    case MixBlendMode::ColorDodge: return SkBlendMode::kColorDodge;
    case MixBlendMode::ColorBurn: return SkBlendMode::kColorBurn;
    case MixBlendMode::HardLight: return SkBlendMode::kHardLight;
    case MixBlendMode::SoftLight: return SkBlendMode::kSoftLight;
    case MixBlendMode::Difference: return SkBlendMode::kDifference;
    case MixBlendMode::Exclusion: return SkBlendMode::kExclusion;
    case MixBlendMode::Hue: return SkBlendMode::kHue;
    case MixBlendMode::Saturation: return SkBlendMode::kSaturation;
    case MixBlendMode::Color: return SkBlendMode::kColor;
    case MixBlendMode::Luminosity: return SkBlendMode::kLuminosity;
  }
  UTILS_UNREACHABLE();
}

}  // namespace


void RendererSkia::pushIsolatedLayer(double opacity, MixBlendMode blendMode) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  SkPaint layerPaint;
  layerPaint.setAlphaf(NarrowToFloat(opacity));
  if (blendMode != MixBlendMode::Normal) {
    layerPaint.setBlendMode(toSkBlendMode(blendMode));
  }
  currentCanvas_->saveLayer(nullptr, &layerPaint);
}

void RendererSkia::popIsolatedLayer() {
  if (currentCanvas_ == nullptr) {
    return;
  }

  currentCanvas_->restore();
}

void RendererSkia::pushMask(const std::optional<Boxd>& maskBounds) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  const int width = static_cast<int>(viewport_.size.x * viewport_.devicePixelRatio);
  const int height = static_cast<int>(viewport_.size.y * viewport_.devicePixelRatio);
  sk_sp<SkSurface> maskSurface = SkSurfaces::Raster(
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
  if (maskSurface == nullptr) {
    return;
  }

  SkCanvas* maskCanvas = maskSurface->getCanvas();
  maskCanvas->clear(SK_ColorTRANSPARENT);
  for (const ClipStackEntry& entry : activeClips_) {
    maskCanvas->save();
    maskCanvas->setMatrix(entry.matrix);
    applyClipToCanvas(maskCanvas, entry.clip);
  }

  maskCanvas->setMatrix(currentCanvas_->getTotalMatrix());
  if (maskBounds.has_value()) {
    maskCanvas->clipRect(toSkia(*maskBounds), SkClipOp::kIntersect, true);
  }

  MaskLayerState state;
  state.maskSurface = std::move(maskSurface);
  state.parentCanvas = currentCanvas_;
  currentCanvas_ = maskCanvas;
  maskLayerStack_.push_back(std::move(state));
}

void RendererSkia::transitionMaskToContent() {
  if (currentCanvas_ == nullptr || maskLayerStack_.empty()) {
    return;
  }

  MaskLayerState& state = maskLayerStack_.back();
  const int width = static_cast<int>(viewport_.size.x * viewport_.devicePixelRatio);
  const int height = static_cast<int>(viewport_.size.y * viewport_.devicePixelRatio);

  // Read the mask surface and convert to a luminance alpha mask.
  // SVG luminance formula: L = 0.2126*R + 0.7152*G + 0.0722*B, scaled by alpha.
  {
    const SkImageInfo info =
        SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    if (state.maskSurface->readPixels(info, rgba.data(), static_cast<size_t>(width) * 4, 0, 0)) {
      const size_t pixelCount = static_cast<size_t>(width) * height;
      state.maskAlpha.resize(pixelCount);
      for (size_t i = 0; i < pixelCount; ++i) {
        // Pixels are premultiplied RGBA. Un-premultiply to get straight alpha,
        // compute luminance, then multiply by alpha for the final mask value.
        const float r = rgba[i * 4 + 0];
        const float g = rgba[i * 4 + 1];
        const float b = rgba[i * 4 + 2];
        const float a = rgba[i * 4 + 3];
        // For premultiplied input, luminance of the straight-alpha color is:
        //   L = (0.2126*R + 0.7152*G + 0.0722*B) / A * A = 0.2126*R + 0.7152*G + 0.0722*B
        // (premultiplied R,G,B already have alpha baked in)
        const float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        state.maskAlpha[i] = static_cast<uint8_t>(std::clamp(luminance, 0.0f, 255.0f));
      }
    }
  }

  state.contentSurface = SkSurfaces::Raster(
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
  if (state.contentSurface == nullptr) {
    currentCanvas_ = state.parentCanvas;
    return;
  }

  SkCanvas* contentCanvas = state.contentSurface->getCanvas();
  contentCanvas->clear(SK_ColorTRANSPARENT);
  for (const ClipStackEntry& entry : activeClips_) {
    contentCanvas->save();
    contentCanvas->setMatrix(entry.matrix);
    applyClipToCanvas(contentCanvas, entry.clip);
  }

  contentCanvas->setMatrix(state.parentCanvas->getTotalMatrix());
  currentCanvas_ = contentCanvas;
}

void RendererSkia::popMask() {
  if (currentCanvas_ == nullptr || maskLayerStack_.empty()) {
    return;
  }

  const int width = static_cast<int>(viewport_.size.x * viewport_.devicePixelRatio);
  const int height = static_cast<int>(viewport_.size.y * viewport_.devicePixelRatio);

  MaskLayerState state = std::move(maskLayerStack_.back());
  maskLayerStack_.pop_back();

  currentCanvas_ = state.parentCanvas;
  if (state.contentSurface == nullptr) {
    return;
  }

  // Read content pixels and apply the luminance mask by multiplying each
  // pixel's alpha channel by the corresponding mask alpha value.
  const SkImageInfo info =
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  std::vector<uint8_t> contentPixels(static_cast<size_t>(width) * height * 4);
  if (!state.contentSurface->readPixels(info, contentPixels.data(), static_cast<size_t>(width) * 4,
                                        0, 0)) {
    return;
  }

  if (!state.maskAlpha.empty()) {
    const size_t pixelCount = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixelCount; ++i) {
      const uint16_t maskA = state.maskAlpha[i];
      // Scale all premultiplied RGBA channels by mask alpha / 255.
      contentPixels[i * 4 + 0] =
          static_cast<uint8_t>((contentPixels[i * 4 + 0] * maskA + 127) / 255);
      contentPixels[i * 4 + 1] =
          static_cast<uint8_t>((contentPixels[i * 4 + 1] * maskA + 127) / 255);
      contentPixels[i * 4 + 2] =
          static_cast<uint8_t>((contentPixels[i * 4 + 2] * maskA + 127) / 255);
      contentPixels[i * 4 + 3] =
          static_cast<uint8_t>((contentPixels[i * 4 + 3] * maskA + 127) / 255);
    }
  }

  const SkPixmap pixmap(info, contentPixels.data(), static_cast<size_t>(width) * 4);
  sk_sp<SkImage> maskedImage = SkImages::RasterFromPixmapCopy(pixmap);
  if (maskedImage == nullptr) {
    return;
  }

  currentCanvas_->save();
  currentCanvas_->resetMatrix();
  currentCanvas_->drawImage(maskedImage, 0.0f, 0.0f);
  currentCanvas_->restore();
}

void RendererSkia::beginPatternTile(const Boxd& tileRect, const Transformd& targetFromPattern) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  PatternState state;
  state.targetFromPattern = targetFromPattern;
  state.savedCanvas = currentCanvas_;
  state.recorder = std::make_unique<SkPictureRecorder>();
  currentCanvas_ = state.recorder->beginRecording(toSkia(tileRect));
  patternStack_.push_back(std::move(state));
}

void RendererSkia::endPatternTile(bool forStroke) {
  if (patternStack_.empty()) {
    return;
  }

  PatternState state = std::move(patternStack_.back());
  patternStack_.pop_back();

  currentCanvas_ = state.savedCanvas;

  const SkMatrix skTargetFromPattern = toSkiaMatrix(state.targetFromPattern);
  sk_sp<SkPicture> picture = state.recorder->finishRecordingAsPicture();
  state.recorder.reset();

  if (picture == nullptr) {
    return;
  }

  const SkRect tileRect = picture->cullRect();
  SkPaint patternPaint;
  patternPaint.setAntiAlias(antialias_);
  patternPaint.setShader(picture->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat,
                                             SkFilterMode::kLinear, &skTargetFromPattern,
                                             &tileRect));

  if (forStroke) {
    patternStrokePaint_ = std::move(patternPaint);
  } else {
    patternFillPaint_ = std::move(patternPaint);
  }
}

void RendererSkia::setPaint(const PaintParams& paint) {
  paint_ = paint;
  paintOpacity_ = paint.opacity;
}

void RendererSkia::drawPath(const PathShape& path, const StrokeParams& stroke) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  if (verbose_) {
    const bool isSolid = std::holds_alternative<PaintServer::Solid>(paint_.fill);
    const SkMatrix m = currentCanvas_->getTotalMatrix();
    std::cout << "[Skia::drawPath] saveCount=" << currentCanvas_->getSaveCount() << " matrix=["
              << m.getScaleX() << "," << m.getScaleY() << "," << m.getTranslateX() << ","
              << m.getTranslateY() << "]"
              << " bounds=" << path.path.bounds() << " fillOpacity=" << paint_.fillOpacity
              << " fillIsSolid=" << isSolid << " isRef="
              << std::holds_alternative<components::PaintResolvedReference>(paint_.fill)
              << " isNone=" << std::holds_alternative<PaintServer::None>(paint_.fill);
    if (isSolid) {
      const auto& solid = std::get<PaintServer::Solid>(paint_.fill);
      std::cout << " color=" << solid.color;
    }
    std::cout << "\n";
  }

  SkPath skPath = toSkia(path.path);
  if (path.fillRule == FillRule::EvenOdd) {
    skPath.setFillType(SkPathFillType::kEvenOdd);
  }

  FilterLayerState* filterLayerState = currentFilterLayerState();
  if (std::optional<SkPaint> fillPaint = makeFillPaint(path.path.bounds())) {
    currentCanvas_->drawPath(skPath, *fillPaint);
    if (filterLayerState != nullptr && filterLayerState->fillPaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->fillPaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawPath(skPath, *fillPaint); });
    }
  }

  // Apply pathLength scaling to dash arrays if needed.
  StrokeParams adjustedStroke = stroke;
  if (!adjustedStroke.dashArray.empty() && adjustedStroke.pathLength > 0.0 &&
      !NearZero(adjustedStroke.pathLength)) {
    const double skiaLength = SkPathMeasure(skPath, false).getLength();
    const double dashUnitsScale = skiaLength / adjustedStroke.pathLength;
    for (double& dash : adjustedStroke.dashArray) {
      dash *= dashUnitsScale;
    }
    adjustedStroke.dashOffset *= dashUnitsScale;
  }

  if (std::optional<SkPaint> strokePaint = makeStrokePaint(path.path.bounds(), adjustedStroke)) {
    currentCanvas_->drawPath(skPath, *strokePaint);
    if (filterLayerState != nullptr && filterLayerState->strokePaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->strokePaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawPath(skPath, *strokePaint); });
    }
  }
}

void RendererSkia::drawRect(const Boxd& rect, const StrokeParams& stroke) {
  const SkRect skRect = toSkia(rect);
  FilterLayerState* filterLayerState = currentFilterLayerState();
  if (std::optional<SkPaint> fillPaint = makeFillPaint(rect)) {
    currentCanvas_->drawRect(skRect, *fillPaint);
    if (filterLayerState != nullptr && filterLayerState->fillPaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->fillPaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawRect(skRect, *fillPaint); });
    }
  }

  if (std::optional<SkPaint> strokePaint = makeStrokePaint(rect, stroke)) {
    currentCanvas_->drawRect(skRect, *strokePaint);
    if (filterLayerState != nullptr && filterLayerState->strokePaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->strokePaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawRect(skRect, *strokePaint); });
    }
  }
}

void RendererSkia::drawEllipse(const Boxd& bounds, const StrokeParams& stroke) {
  SkPath ellipse;
  ellipse.addOval(toSkia(bounds));
  FilterLayerState* filterLayerState = currentFilterLayerState();
  if (std::optional<SkPaint> fillPaint = makeFillPaint(bounds)) {
    currentCanvas_->drawPath(ellipse, *fillPaint);
    if (filterLayerState != nullptr && filterLayerState->fillPaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->fillPaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawPath(ellipse, *fillPaint); });
    }
  }

  if (std::optional<SkPaint> strokePaint = makeStrokePaint(bounds, stroke)) {
    currentCanvas_->drawPath(ellipse, *strokePaint);
    if (filterLayerState != nullptr && filterLayerState->strokePaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->strokePaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawPath(ellipse, *strokePaint); });
    }
  }
}

void RendererSkia::drawImage(const ImageResource& image, const ImageParams& params) {
  if (currentCanvas_ == nullptr || image.data.empty()) {
    return;
  }

  SkImageInfo info =
      SkImageInfo::Make(image.width, image.height, SkColorType::kRGBA_8888_SkColorType,
                        SkAlphaType::kPremul_SkAlphaType);
  const SkPixmap pixmap(info, image.data.data(), static_cast<size_t>(image.width * 4));
  sk_sp<SkImage> skImage = SkImages::RasterFromPixmapCopy(pixmap);
  if (skImage == nullptr) {
    return;
  }

  SkPaint paint = basePaint(antialias_, params.opacity * paintOpacity_);
  const SkSamplingOptions sampling(params.imageRenderingPixelated ? SkFilterMode::kNearest
                                                                  : SkFilterMode::kLinear);

  currentCanvas_->drawImageRect(skImage, toSkia(params.targetRect), sampling, &paint);
}

void RendererSkia::drawText(Registry& registry, const components::ComputedTextComponent& text,
                            const TextParams& params) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  const auto makeSolidFillPaint = [&](const css::Color& color) {
    SkPaint paint = basePaint(antialias_, params.opacity * paintOpacity_);
    paint.setStyle(SkPaint::kFill_Style);

    css::RGBA rgba = color.rgba();
    rgba.a = static_cast<uint8_t>(
        std::round(static_cast<double>(rgba.a) * params.opacity * paintOpacity_));
    paint.setColor(toSkia(rgba));
    return paint;
  };

  // Resolve stroke paint.
  const bool hasStroke = params.strokeParams.strokeWidth > 0.0;

  // Resolve typeface: try system fonts first, then @font-face data, then fallback.
  const SmallVector<RcString, 1>& families = params.fontFamilies;
  const std::string familyName = families.empty() ? "" : families[0].str();
  sk_sp<SkTypeface> typeface = fontMgr_->matchFamilyStyle(familyName.c_str(), SkFontStyle());
  if (!typeface) {
    // Try @font-face declarations for this family.
    for (const auto& face : params.fontFaces) {
      if (face.familyName != familyName) {
        continue;
      }
      for (const auto& source : face.sources) {
        if (source.kind == css::FontFaceSource::Kind::Data) {
          const auto& dataPtr =
              std::get<std::shared_ptr<const std::vector<uint8_t>>>(source.payload);
          typeface = fontMgr_->makeFromData(SkData::MakeWithCopy(dataPtr->data(), dataPtr->size()));
          if (typeface) {
            break;
          }
        }
      }
      if (typeface) {
        break;
      }
    }
  }
  if (!typeface) {
    typeface = fontMgr_->makeFromData(SkData::MakeWithoutCopy(
        embedded::kPublicSansMediumOtf.data(), embedded::kPublicSansMediumOtf.size()));
  }

  const SkScalar fontSizePx = static_cast<SkScalar>(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));

  // Use FontManager for layout and drawGlyphs() for rendering. This ensures
  // identical text positioning across all renderer backends.
  {
    if (!registry.ctx().contains<FontManager>() || !registry.ctx().contains<TextEngine>()) {
      return;
    }

    auto& fontManager = registry.ctx().get<FontManager>();
    auto& textEngine = registry.ctx().get<TextEngine>();

    // Use cached layout runs from ComputedTextGeometryComponent when available.
    std::vector<TextRun> runs;
    if (params.textRootEntity != entt::null) {
      if (const auto* cache =
              registry.try_get<components::ComputedTextGeometryComponent>(params.textRootEntity)) {
        runs = cache->runs;
      }
    }
    if (runs.empty()) {
      const TextLayoutParams layoutParams = toTextLayoutParams(params);
      runs = textEngine.layout(text, layoutParams);
    }

    Boxd textBounds;
    {
      double minX = std::numeric_limits<double>::max();
      double minY = std::numeric_limits<double>::max();
      double maxX = std::numeric_limits<double>::lowest();
      double maxY = std::numeric_limits<double>::lowest();
      for (size_t boundsRunIndex = 0; boundsRunIndex < runs.size(); ++boundsRunIndex) {
        const auto& boundsRun = runs[boundsRunIndex];

        float runFontSizePx = fontSizePx;
        if (boundsRunIndex < text.spans.size() &&
            text.spans[boundsRunIndex].fontSize.value != 0.0) {
          runFontSizePx = static_cast<float>(text.spans[boundsRunIndex].fontSize.toPixels(
              params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
        }

        const float runScale =
            boundsRun.font ? textEngine.scaleForPixelHeight(boundsRun.font, runFontSizePx) : 0.0f;
        double emTop = static_cast<double>(runFontSizePx);
        double emBottom = 0.0;
        if (boundsRun.font && runScale > 0.0f) {
          const FontVMetrics metrics = textEngine.fontVMetrics(boundsRun.font);
          emTop = static_cast<double>(metrics.ascent) * runScale;
          emBottom = -static_cast<double>(metrics.descent) * runScale;
        }

        for (const auto& glyph : boundsRun.glyphs) {
          if (glyph.glyphIndex == 0) {
            continue;
          }

          minX = std::min(minX, glyph.xPosition);
          maxX = std::max(maxX, glyph.xPosition + glyph.xAdvance);
          minY = std::min(minY, glyph.yPosition - emTop);
          maxY = std::max(maxY, glyph.yPosition + emBottom);
        }
      }

      if (minX < maxX && minY < maxY) {
        textBounds = Boxd({minX, minY}, {maxX, maxY});
      }
    }

    std::optional<SkPaint> baseFillPaint = makeFillPaint(textBounds);
    std::optional<SkPaint> baseStrokePaint;
    if (hasStroke) {
      baseStrokePaint = makeStrokePaint(textBounds, params.strokeParams);
    }

    for (size_t runIndex = 0; runIndex < runs.size(); ++runIndex) {
      const auto& run = runs[runIndex];
      if (run.glyphs.empty()) {
        continue;
      }

      float spanFontSizePx = fontSizePx;
      if (runIndex < text.spans.size() && text.spans[runIndex].fontSize.value != 0.0) {
        spanFontSizePx = static_cast<float>(text.spans[runIndex].fontSize.toPixels(
            params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
      }

      sk_sp<SkTypeface> shapedTypeface = typeface;
      if (run.font) {
        const auto fontBytes = fontManager.fontData(run.font);
        if (!fontBytes.empty()) {
          if (sk_sp<SkTypeface> face = fontMgr_->makeFromData(
                  SkData::MakeWithoutCopy(fontBytes.data(), fontBytes.size()))) {
            shapedTypeface = std::move(face);
          }
        }
      }

      SkFont shapedFont(shapedTypeface, spanFontSizePx);
      shapedFont.setEdging(SkFont::Edging::kSubpixelAntiAlias);

      std::optional<SkPaint> spanFillPaint = baseFillPaint;
      std::optional<SkPaint> spanStrokePaint = baseStrokePaint;
      if (runIndex < text.spans.size()) {
        const auto& span = text.spans[runIndex];
        const css::RGBA spanCurrentColor = paint_.currentColor.rgba();
        const float spanFillOpacity = NarrowToFloat(paint_.fillOpacity);
        const float spanStrokeOpacity = NarrowToFloat(paint_.strokeOpacity);

        if (const auto* solid = std::get_if<PaintServer::Solid>(&span.resolvedFill)) {
          SkPaint paint = makeSolidFillPaint(
              css::Color(solid->color.resolve(spanCurrentColor, spanFillOpacity)));
          paint.setAlphaf(NarrowToFloat(paint.getAlphaf() * static_cast<float>(span.opacity)));
          spanFillPaint = paint;
        } else if (const auto* ref =
                       std::get_if<components::PaintResolvedReference>(&span.resolvedFill)) {
          const float combinedOpacity = spanFillOpacity * static_cast<float>(span.opacity);
          if (std::optional<SkPaint> gradient =
                  instantiateGradientPaint(*ref, textBounds, paint_.viewBox, spanCurrentColor,
                                           combinedOpacity, antialias_)) {
            spanFillPaint = std::move(*gradient);
          } else if (ref->fallback.has_value()) {
            SkPaint paint = makeSolidFillPaint(
                css::Color(ref->fallback->resolve(spanCurrentColor, spanFillOpacity)));
            paint.setAlphaf(NarrowToFloat(paint.getAlphaf() * static_cast<float>(span.opacity)));
            spanFillPaint = paint;
          } else {
            // Keep inherited paint for non-gradient refs such as patterns.
          }
        } else if (span.opacity < 1.0 && spanFillPaint.has_value()) {
          SkPaint paint = makeSolidFillPaint(params.fillColor);
          paint.setAlphaf(NarrowToFloat(paint.getAlphaf() * static_cast<float>(span.opacity)));
          spanFillPaint = paint;
        }

        if (span.strokeWidth > 0.0) {
          if (const auto* solid = std::get_if<PaintServer::Solid>(&span.resolvedStroke)) {
            SkPaint paint = basePaint(antialias_, params.opacity * paintOpacity_);
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(NarrowToFloat(span.strokeWidth));
            paint.setStrokeMiter(NarrowToFloat(span.strokeMiterLimit));
            paint.setStrokeCap(toSkia(span.strokeLinecap));
            paint.setStrokeJoin(toSkia(span.strokeLinejoin));
            css::RGBA rgba = solid->color.resolve(spanCurrentColor, spanStrokeOpacity);
            rgba.a = static_cast<uint8_t>(
                std::round(static_cast<double>(rgba.a) * static_cast<float>(span.opacity)));
            paint.setColor(toSkia(rgba));
            spanStrokePaint = paint;
          } else if (const auto* ref =
                         std::get_if<components::PaintResolvedReference>(&span.resolvedStroke)) {
            const float combinedOpacity = spanStrokeOpacity * static_cast<float>(span.opacity);
            if (std::optional<SkPaint> gradient =
                    instantiateGradientPaint(*ref, textBounds, paint_.viewBox, spanCurrentColor,
                                             combinedOpacity, antialias_)) {
              gradient->setStyle(SkPaint::kStroke_Style);
              gradient->setStrokeWidth(NarrowToFloat(span.strokeWidth));
              gradient->setStrokeMiter(NarrowToFloat(span.strokeMiterLimit));
              gradient->setStrokeCap(toSkia(span.strokeLinecap));
              gradient->setStrokeJoin(toSkia(span.strokeLinejoin));
              spanStrokePaint = std::move(*gradient);
            } else if (ref->fallback.has_value()) {
              SkPaint paint = basePaint(antialias_, params.opacity * paintOpacity_);
              paint.setStyle(SkPaint::kStroke_Style);
              paint.setStrokeWidth(NarrowToFloat(span.strokeWidth));
              paint.setStrokeMiter(NarrowToFloat(span.strokeMiterLimit));
              paint.setStrokeCap(toSkia(span.strokeLinecap));
              paint.setStrokeJoin(toSkia(span.strokeLinejoin));
              css::RGBA rgba = ref->fallback->resolve(spanCurrentColor, spanStrokeOpacity);
              rgba.a = static_cast<uint8_t>(
                  std::round(static_cast<double>(rgba.a) * static_cast<float>(span.opacity)));
              paint.setColor(toSkia(rgba));
              spanStrokePaint = paint;
            } else {
              // Keep inherited paint for non-gradient refs such as patterns.
            }
          } else {
            spanStrokePaint.reset();
          }
        } else {
          spanStrokePaint.reset();
        }
      }

      const bool isBitmapFont = run.font && textEngine.isBitmapOnly(run.font);
      const float glyphScale =
          run.font ? textEngine.scaleForPixelHeight(run.font, spanFontSizePx) : 0.0f;

      if (run.font != FontHandle() && !isBitmapFont && glyphScale > 0.0f) {
        for (const auto& glyph : run.glyphs) {
          if (glyph.glyphIndex == 0) {
            continue;
          }

          PathSpline glyphPath =
              textEngine.glyphOutline(run.font, glyph.glyphIndex, glyphScale * glyph.fontSizeScale);
          if (glyphPath.empty()) {
            continue;
          }

          if (glyph.stretchScaleX != 1.0f || glyph.stretchScaleY != 1.0f) {
            glyphPath = transformPathSpline(
                glyphPath, Transformd::Scale(glyph.stretchScaleX, glyph.stretchScaleY));
          }

          Transformd glyphFromLocal = Transformd::Translate(glyph.xPosition, glyph.yPosition);
          if (glyph.rotateDegrees != 0.0) {
            glyphFromLocal =
                Transformd::Rotate(glyph.rotateDegrees * std::numbers::pi_v<double> / 180.0) *
                glyphFromLocal;
          }

          const SkPath skPath = toSkia(transformPathSpline(glyphPath, glyphFromLocal));
          if (spanFillPaint.has_value()) {
            currentCanvas_->drawPath(skPath, *spanFillPaint);
          }
          if (spanStrokePaint.has_value()) {
            currentCanvas_->drawPath(skPath, *spanStrokePaint);
          }
        }
      } else if (isBitmapFont && run.font) {
        // Bitmap font (color emoji): draw each glyph as an image.
        for (const auto& glyph : run.glyphs) {
          if (glyph.glyphIndex == 0) {
            continue;
          }

          auto bitmap = textEngine.bitmapGlyph(run.font, glyph.glyphIndex, glyphScale);
          if (!bitmap) {
            continue;
          }

          // Premultiply alpha for Skia.
          std::vector<uint8_t> premul = PremultiplyRgba(bitmap->rgbaPixels);
          SkImageInfo info = SkImageInfo::MakeN32Premul(bitmap->width, bitmap->height);
          sk_sp<SkImage> skImage = SkImages::RasterFromPixmapCopy(
              SkPixmap(info, premul.data(), static_cast<size_t>(bitmap->width) * 4));
          if (!skImage) {
            continue;
          }

          const double targetX = glyph.xPosition + bitmap->bearingX;
          const double targetY = glyph.yPosition - bitmap->bearingY;
          const double targetW =
              static_cast<double>(bitmap->width) * bitmap->scale * glyph.stretchScaleX;
          const double targetH =
              static_cast<double>(bitmap->height) * bitmap->scale * glyph.stretchScaleY;

          SkRect dstRect = SkRect::MakeXYWH(NarrowToFloat(targetX), NarrowToFloat(targetY),
                                             NarrowToFloat(targetW), NarrowToFloat(targetH));
          SkPaint imgPaint;
          imgPaint.setAntiAlias(antialias_);
          currentCanvas_->drawImageRect(skImage, dstRect, SkSamplingOptions(), &imgPaint);
        }
      } else {
        const auto glyphCount = static_cast<int>(run.glyphs.size());
        std::vector<SkGlyphID> skGlyphs(run.glyphs.size());
        std::vector<SkPoint> skPositions(run.glyphs.size());

        for (size_t i = 0; i < run.glyphs.size(); ++i) {
          skGlyphs[i] = static_cast<SkGlyphID>(run.glyphs[i].glyphIndex);
          skPositions[i] = SkPoint::Make(NarrowToFloat(run.glyphs[i].xPosition),
                                         NarrowToFloat(run.glyphs[i].yPosition));
        }

        const SkPoint origin = SkPoint::Make(0, 0);
        const bool hasPerGlyphTransform =
            std::any_of(run.glyphs.begin(), run.glyphs.end(), [](const auto& glyph) {
              return glyph.rotateDegrees != 0.0 || std::abs(glyph.fontSizeScale - 1.0f) > 1e-6f ||
                     std::abs(glyph.stretchScaleX - 1.0f) > 1e-6f ||
                     std::abs(glyph.stretchScaleY - 1.0f) > 1e-6f;
            });

        if (hasPerGlyphTransform) {
          for (int i = 0; i < glyphCount; ++i) {
            currentCanvas_->save();
            currentCanvas_->translate(skPositions[i].x(), skPositions[i].y());
            if (std::abs(run.glyphs[i].fontSizeScale - 1.0f) > 1e-6f) {
              currentCanvas_->scale(run.glyphs[i].fontSizeScale, run.glyphs[i].fontSizeScale);
            }
            if (std::abs(run.glyphs[i].stretchScaleX - 1.0f) > 1e-6f ||
                std::abs(run.glyphs[i].stretchScaleY - 1.0f) > 1e-6f) {
              currentCanvas_->scale(run.glyphs[i].stretchScaleX, run.glyphs[i].stretchScaleY);
            }
            currentCanvas_->rotate(NarrowToFloat(run.glyphs[i].rotateDegrees));
            if (spanFillPaint.has_value()) {
              currentCanvas_->drawGlyphs(1, &skGlyphs[i], &origin, origin, shapedFont,
                                         *spanFillPaint);
            }
            if (spanStrokePaint.has_value()) {
              currentCanvas_->drawGlyphs(1, &skGlyphs[i], &origin, origin, shapedFont,
                                         *spanStrokePaint);
            }
            currentCanvas_->restore();
          }
        } else {
          if (spanFillPaint.has_value()) {
            currentCanvas_->drawGlyphs(glyphCount, skGlyphs.data(), skPositions.data(), origin,
                                       shapedFont, *spanFillPaint);
          }
          if (spanStrokePaint.has_value()) {
            currentCanvas_->drawGlyphs(glyphCount, skGlyphs.data(), skPositions.data(), origin,
                                       shapedFont, *spanStrokePaint);
          }
        }
      }

      const bool hasSpan = runIndex < text.spans.size();
      const TextDecoration spanDecoration =
          hasSpan ? text.spans[runIndex].textDecoration : params.textDecoration;

      // Draw text-decoration lines. Per CSS Text Decoration §3, decoration uses the paint and
      // font metrics of the element that declared text-decoration, not the current span's.
      if (spanDecoration != TextDecoration::None && !run.glyphs.empty() && run.font && hasSpan) {
        const auto& span = text.spans[runIndex];

        const float decoFontSizePx =
            span.decorationFontSizePx > 0.0f ? span.decorationFontSizePx : spanFontSizePx;
        const float decoScale = textEngine.scaleForPixelHeight(run.font, decoFontSizePx);
        const float decoEmScale = textEngine.scaleForEmToPixels(run.font, decoFontSizePx);

        const FontVMetrics vmetrics = textEngine.fontVMetrics(run.font);
        const int ascent = vmetrics.ascent;
        const int descent = vmetrics.descent;

        double fontUnderlinePos = 0.0;
        double fontUnderlineThick = 0.0;
        if (auto underline = textEngine.underlineMetrics(run.font)) {
          fontUnderlinePos = underline->position;
          fontUnderlineThick = underline->thickness;
        }

        double fontStrikePos = 0.0;
        double fontStrikeThick = 0.0;
        if (auto strike = textEngine.strikeoutMetrics(run.font)) {
          fontStrikePos = strike->position;
          fontStrikeThick = strike->thickness;
        }

        const double thickness = fontUnderlineThick > 0.0
                                     ? fontUnderlineThick * decoEmScale
                                     : static_cast<double>(ascent - descent) * decoScale / 18.0;

        std::optional<SkPaint> decoFillPaint;
        const css::RGBA spanCurrentColor = paint_.currentColor.rgba();
        if (const auto* solid = std::get_if<PaintServer::Solid>(&span.resolvedDecorationFill)) {
          SkPaint paint = makeSolidFillPaint(css::Color(
              solid->color.resolve(spanCurrentColor, NarrowToFloat(span.decorationFillOpacity))));
          paint.setAlphaf(NarrowToFloat(paint.getAlphaf() * static_cast<float>(span.opacity)));
          decoFillPaint = paint;
        } else if (const auto* ref = std::get_if<components::PaintResolvedReference>(
                       &span.resolvedDecorationFill)) {
          const float combinedOpacity =
              NarrowToFloat(span.decorationFillOpacity) * static_cast<float>(span.opacity);
          if (std::optional<SkPaint> gradient =
                  instantiateGradientPaint(*ref, textBounds, paint_.viewBox, spanCurrentColor,
                                           combinedOpacity, antialias_)) {
            decoFillPaint = std::move(*gradient);
          }
        }
        if (!decoFillPaint.has_value()) {
          decoFillPaint = spanFillPaint;
        }

        std::optional<SkPaint> decoStrokePaint;
        if (const auto* solid = std::get_if<PaintServer::Solid>(&span.resolvedDecorationStroke)) {
          SkPaint paint = basePaint(antialias_, params.opacity * paintOpacity_);
          paint.setStyle(SkPaint::kStroke_Style);
          paint.setStrokeWidth(NarrowToFloat(span.decorationStrokeWidth));
          css::RGBA rgba =
              solid->color.resolve(spanCurrentColor, NarrowToFloat(span.decorationStrokeOpacity));
          rgba.a = static_cast<uint8_t>(
              std::round(static_cast<double>(rgba.a) * static_cast<float>(span.opacity)));
          paint.setColor(toSkia(rgba));
          decoStrokePaint = paint;
        } else if (const auto* ref = std::get_if<components::PaintResolvedReference>(
                       &span.resolvedDecorationStroke)) {
          const float combinedOpacity =
              NarrowToFloat(span.decorationStrokeOpacity) * static_cast<float>(span.opacity);
          if (std::optional<SkPaint> gradient =
                  instantiateGradientPaint(*ref, textBounds, paint_.viewBox, spanCurrentColor,
                                           combinedOpacity, antialias_)) {
            gradient->setStyle(SkPaint::kStroke_Style);
            gradient->setStrokeWidth(NarrowToFloat(span.decorationStrokeWidth));
            decoStrokePaint = std::move(*gradient);
          }
        }

        const bool hasRotation =
            std::any_of(run.glyphs.begin(), run.glyphs.end(),
                        [](const auto& glyph) { return glyph.rotateDegrees != 0.0; });

        for (TextDecoration decoType :
             {TextDecoration::Underline, TextDecoration::Overline, TextDecoration::LineThrough}) {
          if (!hasFlag(spanDecoration, decoType)) {
            continue;
          }

          double decoThickness = thickness;
          if (decoType == TextDecoration::LineThrough && fontStrikeThick > 0.0) {
            decoThickness = fontStrikeThick * decoEmScale;
          }

          double decoOffsetY = 0.0;
          if (decoType == TextDecoration::Underline) {
            decoOffsetY = fontUnderlinePos != 0.0 ? -fontUnderlinePos * decoEmScale
                                                  : -static_cast<double>(descent) * decoScale * 0.4;
          } else if (decoType == TextDecoration::Overline) {
            decoOffsetY = -static_cast<double>(ascent) * decoScale;
          } else if (decoType == TextDecoration::LineThrough) {
            decoOffsetY = fontStrikePos != 0.0 ? -fontStrikePos * decoEmScale
                                               : -static_cast<double>(ascent) * decoScale * 0.35;
          }

          double decoTopY = decoOffsetY - decoThickness / 2.0;

          const bool hasMultipleDecorationLines =
              (hasFlag(spanDecoration, TextDecoration::Underline) ? 1 : 0) +
                  (hasFlag(spanDecoration, TextDecoration::Overline) ? 1 : 0) +
                  (hasFlag(spanDecoration, TextDecoration::LineThrough) ? 1 : 0) >
              1;
          if (span.decorationDeclarationCount == 1 && hasMultipleDecorationLines) {
            if (decoType == TextDecoration::Overline) {
              decoTopY += decoThickness * 1.5;
            } else if (decoType == TextDecoration::LineThrough) {
              decoTopY -= decoThickness;
            }
          }

          auto drawDecoPath = [&](const PathSpline& spline) {
            const SkPath path = toSkia(spline);
            if (decoFillPaint.has_value()) {
              currentCanvas_->drawPath(path, *decoFillPaint);
            }
            if (decoStrokePaint.has_value() && span.decorationStrokeWidth > 0.0) {
              currentCanvas_->drawPath(path, *decoStrokePaint);
            }
          };

          const auto isRenderedGlyph = [](const auto& glyph) {
            return glyph.glyphIndex != 0 && glyph.xAdvance > 0.0;
          };

          if (hasRotation) {
            for (size_t glyphIndex = 0; glyphIndex < run.glyphs.size(); ++glyphIndex) {
              const auto& glyph = run.glyphs[glyphIndex];
              if (!isRenderedGlyph(glyph)) {
                continue;
              }

              double segmentWidth = glyph.xAdvance;
              for (size_t nextIndex = glyphIndex + 1; nextIndex < run.glyphs.size(); ++nextIndex) {
                const auto& nextGlyph = run.glyphs[nextIndex];
                if (!isRenderedGlyph(nextGlyph)) {
                  continue;
                }

                segmentWidth = std::min(segmentWidth, nextGlyph.xPosition - glyph.xPosition);
                break;
              }

              if (segmentWidth <= 0.0) {
                continue;
              }

              PathSpline segmentPath;
              segmentPath.moveTo(Vector2d(0.0, decoTopY));
              segmentPath.lineTo(Vector2d(segmentWidth, decoTopY));
              segmentPath.lineTo(Vector2d(segmentWidth, decoTopY + decoThickness));
              segmentPath.lineTo(Vector2d(0.0, decoTopY + decoThickness));
              segmentPath.closePath();

              Transformd segmentFromLocal = Transformd::Translate(glyph.xPosition, glyph.yPosition);
              if (glyph.rotateDegrees != 0.0) {
                segmentFromLocal =
                    Transformd::Rotate(glyph.rotateDegrees * std::numbers::pi_v<double> / 180.0) *
                    segmentFromLocal;
              }

              drawDecoPath(transformPathSpline(segmentPath, segmentFromLocal));
            }
          } else {
            const auto firstGlyph =
                std::find_if(run.glyphs.begin(), run.glyphs.end(), isRenderedGlyph);
            const auto lastGlyph =
                std::find_if(run.glyphs.rbegin(), run.glyphs.rend(), isRenderedGlyph);
            if (firstGlyph == run.glyphs.end() || lastGlyph == run.glyphs.rend()) {
              continue;
            }

            const double baselineY = firstGlyph->yPosition;
            const bool sameBaseline =
                std::all_of(run.glyphs.begin(), run.glyphs.end(), [&](const auto& glyph) {
                  return !isRenderedGlyph(glyph) || std::abs(glyph.yPosition - baselineY) < 1e-6;
                });

            if (sameBaseline) {
              PathSpline decoPath;
              const double x0 = firstGlyph->xPosition;
              const double x1 = lastGlyph->xPosition + lastGlyph->xAdvance;
              const double y = baselineY + decoTopY;
              decoPath.moveTo(Vector2d(x0, y));
              decoPath.lineTo(Vector2d(x1, y));
              decoPath.lineTo(Vector2d(x1, y + decoThickness));
              decoPath.lineTo(Vector2d(x0, y + decoThickness));
              decoPath.closePath();
              drawDecoPath(decoPath);
            } else {
              PathSpline decoPath;
              for (size_t glyphIndex = 0; glyphIndex < run.glyphs.size(); ++glyphIndex) {
                const auto& glyph = run.glyphs[glyphIndex];
                if (!isRenderedGlyph(glyph)) {
                  continue;
                }

                const double x0 = glyph.xPosition;
                double x1 = glyph.xPosition + glyph.xAdvance;
                for (size_t nextIndex = glyphIndex + 1; nextIndex < run.glyphs.size();
                     ++nextIndex) {
                  const auto& nextGlyph = run.glyphs[nextIndex];
                  if (!isRenderedGlyph(nextGlyph)) {
                    continue;
                  }

                  x1 = std::min(x1, nextGlyph.xPosition);
                  break;
                }

                if (x1 <= x0) {
                  continue;
                }

                const double y = glyph.yPosition + decoTopY;
                decoPath.moveTo(Vector2d(x0, y));
                decoPath.lineTo(Vector2d(x1, y));
                decoPath.lineTo(Vector2d(x1, y + decoThickness));
                decoPath.lineTo(Vector2d(x0, y + decoThickness));
                decoPath.closePath();
              }

              if (!decoPath.empty()) {
                drawDecoPath(decoPath);
              }
            }
          }
        }
      }
    }
  }
}

RendererBitmap RendererSkia::takeSnapshot() const {
  RendererBitmap snapshot;
  snapshot.dimensions = Vector2i(bitmap_.width(), bitmap_.height());
  snapshot.rowBytes = bitmap_.rowBytes();

  if (bitmap_.empty()) {
    return snapshot;
  }

  const size_t size = bitmap_.computeByteSize();
  snapshot.pixels.resize(size);
  const bool copied =
      bitmap_.readPixels(bitmap_.info(), snapshot.pixels.data(), snapshot.rowBytes, 0, 0);
  if (!copied) {
    snapshot.pixels.clear();
    snapshot.dimensions = Vector2i::Zero();
    snapshot.rowBytes = 0;
  }

  return snapshot;
}

std::unique_ptr<RendererInterface> RendererSkia::createOffscreenInstance() const {
  return std::make_unique<RendererSkia>(verbose_);
}

void RendererSkia::draw(SVGDocument& document) {
  RendererDriver driver(*this, verbose_);
  driver.draw(document);
}

sk_sp<SkPicture> RendererSkia::drawIntoSkPicture(SVGDocument& document) {
  SkPictureRecorder recorder;
  const Vector2i renderingSize = document.canvasSize();
  externalCanvas_ = recorder.beginRecording(SkRect::MakeWH(static_cast<SkScalar>(renderingSize.x),
                                                           static_cast<SkScalar>(renderingSize.y)));
  draw(document);
  return recorder.finishRecordingAsPicture();
}

bool RendererSkia::save(const char* filename) {
  const RendererBitmap snapshot = takeSnapshot();
  if (snapshot.empty()) {
    return false;
  }

  return RendererImageIO::writeRgbaPixelsToPngFile(filename, snapshot.pixels, snapshot.dimensions.x,
                                                   snapshot.dimensions.y);
}

std::span<const uint8_t> RendererSkia::pixelData() const {
  return std::span<const uint8_t>(
      bitmap_.computeByteSize() == 0 ? nullptr : static_cast<const uint8_t*>(bitmap_.getPixels()),
      bitmap_.computeByteSize());
}

}  // namespace donner::svg
