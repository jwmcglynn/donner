#include "donner/svg/renderer/RendererTinySkia.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <numbers>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/Length.h"
#include "donner/base/MathUtils.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererImageIO.h"
#ifdef DONNER_TEXT_ENABLED
#ifdef DONNER_TEXT_FULL
#include "donner/svg/renderer/TextShaper.h"
#else
#include "donner/svg/renderer/TextLayout.h"
#endif
#include "donner/svg/resources/FontManager.h"

#define STBTT_DEF extern
#include <stb/stb_truetype.h>
#endif
#include "tiny_skia/Painter.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/shaders/Shaders.h"

namespace donner::svg {

namespace {

const Boxd kUnitPathBounds(Vector2d::Zero(), Vector2d(1, 1));

tiny_skia::Color toTinyColor(const css::RGBA& rgba) {
  return tiny_skia::Color::fromRgba8(rgba.r, rgba.g, rgba.b, rgba.a);
}

tiny_skia::Point toTinyPoint(const Vector2d& value) {
  return tiny_skia::Point::fromXY(NarrowToFloat(value.x), NarrowToFloat(value.y));
}

tiny_skia::Transform toTinyTransform(const Transformd& transform) {
  return tiny_skia::Transform::fromRow(
      NarrowToFloat(transform.data[0]), NarrowToFloat(transform.data[1]),
      NarrowToFloat(transform.data[2]), NarrowToFloat(transform.data[3]),
      NarrowToFloat(transform.data[4]), NarrowToFloat(transform.data[5]));
}

std::optional<tiny_skia::Rect> toTinyRect(const Boxd& box) {
  return tiny_skia::Rect::fromLTRB(NarrowToFloat(box.topLeft.x), NarrowToFloat(box.topLeft.y),
                                   NarrowToFloat(box.bottomRight.x),
                                   NarrowToFloat(box.bottomRight.y));
}

tiny_skia::FillRule toTinyFillRule(FillRule fillRule) {
  switch (fillRule) {
    case FillRule::NonZero: return tiny_skia::FillRule::Winding;
    case FillRule::EvenOdd: return tiny_skia::FillRule::EvenOdd;
  }

  UTILS_UNREACHABLE();
}

tiny_skia::LineCap toTinyLineCap(StrokeLinecap lineCap) {
  switch (lineCap) {
    case StrokeLinecap::Butt: return tiny_skia::LineCap::Butt;
    case StrokeLinecap::Round: return tiny_skia::LineCap::Round;
    case StrokeLinecap::Square: return tiny_skia::LineCap::Square;
  }

  UTILS_UNREACHABLE();
}

tiny_skia::LineJoin toTinyLineJoin(StrokeLinejoin lineJoin) {
  switch (lineJoin) {
    case StrokeLinejoin::Miter: return tiny_skia::LineJoin::Miter;
    case StrokeLinejoin::MiterClip: return tiny_skia::LineJoin::MiterClip;
    case StrokeLinejoin::Round: return tiny_skia::LineJoin::Round;
    case StrokeLinejoin::Bevel: return tiny_skia::LineJoin::Bevel;
    case StrokeLinejoin::Arcs: return tiny_skia::LineJoin::Miter;
  }

  UTILS_UNREACHABLE();
}

tiny_skia::SpreadMode toTinySpreadMode(GradientSpreadMethod spreadMethod) {
  switch (spreadMethod) {
    case GradientSpreadMethod::Pad: return tiny_skia::SpreadMode::Pad;
    case GradientSpreadMethod::Reflect: return tiny_skia::SpreadMode::Reflect;
    case GradientSpreadMethod::Repeat: return tiny_skia::SpreadMode::Repeat;
  }

  UTILS_UNREACHABLE();
}

tiny_skia::Path toTinyPath(const PathSpline& spline) {
  tiny_skia::PathBuilder builder(spline.commands().size(), spline.points().size());
  const std::vector<Vector2d>& points = spline.points();

  for (const PathSpline::Command& command : spline.commands()) {
    switch (command.type) {
      case PathSpline::CommandType::MoveTo: {
        const Vector2d& point = points[command.pointIndex];
        builder.moveTo(NarrowToFloat(point.x), NarrowToFloat(point.y));
        break;
      }
      case PathSpline::CommandType::LineTo: {
        const Vector2d& point = points[command.pointIndex];
        builder.lineTo(NarrowToFloat(point.x), NarrowToFloat(point.y));
        break;
      }
      case PathSpline::CommandType::CurveTo: {
        const Vector2d& control1 = points[command.pointIndex];
        const Vector2d& control2 = points[command.pointIndex + 1];
        const Vector2d& endPoint = points[command.pointIndex + 2];
        builder.cubicTo(NarrowToFloat(control1.x), NarrowToFloat(control1.y),
                        NarrowToFloat(control2.x), NarrowToFloat(control2.y),
                        NarrowToFloat(endPoint.x), NarrowToFloat(endPoint.y));
        break;
      }
      case PathSpline::CommandType::ClosePath: {
        builder.close();
        break;
      }
    }
  }

  return builder.finish().value_or(tiny_skia::Path());
}

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
      case PathSpline::CommandType::ClosePath:
        result.closePath();
        break;
    }
  }

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

inline double resolveGradientCoord(Lengthd value, const Boxd& viewBox, bool numbersArePercent) {
  return toPercent(value, numbersArePercent).toPixels(viewBox, FontMetrics());
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

std::optional<tiny_skia::Shader> instantiateGradientShader(
    const components::PaintResolvedReference& ref, const Boxd& pathBounds, const Boxd& viewBox,
    const css::RGBA& currentColor, float opacity) {
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

  std::vector<tiny_skia::GradientStop> stops;
  stops.reserve(computedGradient->stops.size());
  for (const GradientStop& stop : computedGradient->stops) {
    stops.push_back(tiny_skia::GradientStop::create(
        stop.offset, toTinyColor(stop.color.resolve(currentColor, stop.opacity * opacity))));
  }

  if (stops.empty()) {
    return std::nullopt;
  }

  if (stops.size() == 1) {
    return tiny_skia::Shader(stops.front().color);
  }

  const tiny_skia::Transform shaderTransform = toTinyTransform(gradientFromGradientUnits);

  if (const auto* linear = handle.try_get<components::ComputedLinearGradientComponent>()) {
    const Vector2d start = resolveGradientCoords(linear->x1, linear->y1, bounds, numbersArePercent);
    const Vector2d end = resolveGradientCoords(linear->x2, linear->y2, bounds, numbersArePercent);
    const auto shader = tiny_skia::LinearGradient::create(
        toTinyPoint(start), toTinyPoint(end), std::move(stops),
        toTinySpreadMode(computedGradient->spreadMethod), shaderTransform);
    if (!shader.has_value()) {
      return std::nullopt;
    }

    return std::visit(
        [](auto&& value) -> tiny_skia::Shader {
          return tiny_skia::Shader(std::forward<decltype(value)>(value));
        },
        std::move(*shader));
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
      return tiny_skia::Shader(stops.back().color);
    }

    const double distanceBetweenCenters = (center - focalCenter).length();
    if (distanceBetweenCenters + radius <= focalRadius) {
      return std::nullopt;
    }

    const auto shader = tiny_skia::RadialGradient::create(
        toTinyPoint(focalCenter), NarrowToFloat(focalRadius), toTinyPoint(center),
        NarrowToFloat(radius), std::move(stops), toTinySpreadMode(computedGradient->spreadMethod),
        shaderTransform);
    if (!shader.has_value()) {
      return std::nullopt;
    }

    return std::visit(
        [](auto&& value) -> tiny_skia::Shader {
          return tiny_skia::Shader(std::forward<decltype(value)>(value));
        },
        std::move(*shader));
  }

  return std::nullopt;
}

tiny_skia::Paint makeBasePaint(bool antialias) {
  tiny_skia::Paint paint;
  paint.antiAlias = antialias;
  return paint;
}

std::optional<tiny_skia::Mask> createMaskForSize(std::uint32_t width, std::uint32_t height) {
  return tiny_skia::Mask::fromSize(width, height);
}

void intersectMaskInPlace(tiny_skia::Mask& dst, const tiny_skia::Mask& src) {
  if (dst.size() != src.size()) {
    return;
  }

  std::span<std::uint8_t> dstData = dst.data();
  std::span<const std::uint8_t> srcData = src.data();
  for (std::size_t i = 0; i < dstData.size(); ++i) {
    dstData[i] = std::min(dstData[i], srcData[i]);
  }
}

void unionMaskInPlace(tiny_skia::Mask& dst, const tiny_skia::Mask& src) {
  if (dst.size() != src.size()) {
    return;
  }

  std::span<std::uint8_t> dstData = dst.data();
  std::span<const std::uint8_t> srcData = src.data();
  for (std::size_t i = 0; i < dstData.size(); ++i) {
    dstData[i] = std::max(dstData[i], srcData[i]);
  }
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
  const double scaleX =
      NearZero(tileRect.width()) ? fallbackScale.x : static_cast<double>(pixelWidth) / tileRect.width();
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

void drawRectIntoMask(tiny_skia::Mask& mask, const Boxd& rect, const Transformd& transform,
                      bool antialias) {
  const std::optional<tiny_skia::Rect> tinyRect = toTinyRect(rect);
  if (!tinyRect.has_value()) {
    return;
  }

  const tiny_skia::Path path = tiny_skia::Path::fromRect(*tinyRect);
  mask.fillPath(path, tiny_skia::FillRule::Winding, antialias, toTinyTransform(transform));
}

struct ConvolutionKernel {
  std::vector<float> weights;
  std::vector<std::uint32_t> numerators;
  std::uint32_t divisor = 1;
  int origin = 0;
};

ConvolutionKernel makeGaussianKernel(double sigma) {
  if (sigma <= 0.0) {
    return {{1.0f}, {}, 1u, 0};
  }

  const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0)));
  std::vector<float> weights(static_cast<std::size_t>(radius * 2 + 1));

  const double twoSigmaSquared = 2.0 * sigma * sigma;
  double sum = 0.0;
  for (int i = -radius; i <= radius; ++i) {
    const double weight = std::exp(-(i * i) / twoSigmaSquared);
    weights[static_cast<std::size_t>(i + radius)] = static_cast<float>(weight);
    sum += weight;
  }

  for (float& weight : weights) {
    weight = static_cast<float>(weight / sum);
  }

  return {std::move(weights), {}, 1u, radius};
}

ConvolutionKernel makeBoxKernel(int minOffset, int maxOffset) {
  const int width = maxOffset - minOffset + 1;
  std::vector<std::uint32_t> numerators(static_cast<std::size_t>(width), 1u);
  return {{}, std::move(numerators), static_cast<std::uint32_t>(width), -minOffset};
}

ConvolutionKernel convolveKernels(const ConvolutionKernel& lhs, const ConvolutionKernel& rhs) {
  ConvolutionKernel result;
  result.origin = lhs.origin + rhs.origin;
  result.divisor = lhs.divisor * rhs.divisor;

  if (!lhs.numerators.empty() && !rhs.numerators.empty()) {
    result.numerators.assign(lhs.numerators.size() + rhs.numerators.size() - 1u, 0u);
    for (std::size_t lhsIndex = 0; lhsIndex < lhs.numerators.size(); ++lhsIndex) {
      for (std::size_t rhsIndex = 0; rhsIndex < rhs.numerators.size(); ++rhsIndex) {
        result.numerators[lhsIndex + rhsIndex] += lhs.numerators[lhsIndex] * rhs.numerators[rhsIndex];
      }
    }
  } else {
    result.weights.assign(lhs.weights.size() + rhs.weights.size() - 1u, 0.0f);
    for (std::size_t lhsIndex = 0; lhsIndex < lhs.weights.size(); ++lhsIndex) {
      for (std::size_t rhsIndex = 0; rhsIndex < rhs.weights.size(); ++rhsIndex) {
        result.weights[lhsIndex + rhsIndex] += lhs.weights[lhsIndex] * rhs.weights[rhsIndex];
      }
    }
  }

  return result;
}

ConvolutionKernel makeBoxApproximationKernel(double sigma) {
  if (sigma <= 0.0) {
    return {{1.0f}, {}, 1u, 0};
  }

  const double kWindowScale =
      3.0 * std::sqrt(2.0 * std::numbers::pi_v<double>) / 4.0;
  const int window =
      std::max(1, static_cast<int>(std::floor(sigma * kWindowScale + 0.5)));
  if (window <= 1) {
    return {{1.0f}, {}, 1u, 0};
  }

  if ((window & 1) != 0) {
    const int radius = window / 2;
    const ConvolutionKernel box = makeBoxKernel(-radius, radius);
    return convolveKernels(convolveKernels(box, box), box);
  }

  const int half = window / 2;
  const ConvolutionKernel leftShifted = makeBoxKernel(-half, half - 1);
  const ConvolutionKernel rightShifted = makeBoxKernel(-(half - 1), half);
  const ConvolutionKernel centered = makeBoxKernel(-half, half);
  return convolveKernels(convolveKernels(leftShifted, rightShifted), centered);
}

ConvolutionKernel makeBlurKernel(double sigma) {
  if (sigma < 2.0) {
    return makeGaussianKernel(sigma);
  }

  return makeBoxApproximationKernel(sigma);
}

void convolveHorizontal(const std::vector<std::uint8_t>& src, std::vector<std::uint8_t>& dst,
                        int width, int height, const ConvolutionKernel& kernel) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int channel = 0; channel < 4; ++channel) {
        if (!kernel.numerators.empty()) {
          std::uint64_t sum = 0;
          for (std::size_t kernelIndex = 0; kernelIndex < kernel.numerators.size();
               ++kernelIndex) {
            const int sampleX = x + static_cast<int>(kernelIndex) - kernel.origin;
            if (sampleX < 0 || sampleX >= width) {
              continue;
            }

            const std::size_t srcIndex =
                static_cast<std::size_t>((y * width + sampleX) * 4 + channel);
            sum += static_cast<std::uint64_t>(kernel.numerators[kernelIndex]) * src[srcIndex];
          }

          const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
          dst[dstIndex] = static_cast<std::uint8_t>(std::clamp(
              static_cast<long>((sum + kernel.divisor / 2u) / kernel.divisor), 0L, 255L));
          continue;
        }

        float sum = 0.0f;
        for (std::size_t kernelIndex = 0; kernelIndex < kernel.weights.size(); ++kernelIndex) {
          const int sampleX = x + static_cast<int>(kernelIndex) - kernel.origin;
          if (sampleX < 0 || sampleX >= width) {
            continue;
          }

          const std::size_t srcIndex =
              static_cast<std::size_t>((y * width + sampleX) * 4 + channel);
          sum += kernel.weights[kernelIndex] * static_cast<float>(src[srcIndex]);
        }

        const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
        dst[dstIndex] = static_cast<std::uint8_t>(std::clamp(std::lround(sum), 0L, 255L));
      }
    }
  }
}

void convolveVertical(const std::vector<std::uint8_t>& src, std::vector<std::uint8_t>& dst,
                      int width, int height, const ConvolutionKernel& kernel) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int channel = 0; channel < 4; ++channel) {
        if (!kernel.numerators.empty()) {
          std::uint64_t sum = 0;
          for (std::size_t kernelIndex = 0; kernelIndex < kernel.numerators.size();
               ++kernelIndex) {
            const int sampleY = y + static_cast<int>(kernelIndex) - kernel.origin;
            if (sampleY < 0 || sampleY >= height) {
              continue;
            }

            const std::size_t srcIndex =
                static_cast<std::size_t>((sampleY * width + x) * 4 + channel);
            sum += static_cast<std::uint64_t>(kernel.numerators[kernelIndex]) * src[srcIndex];
          }

          const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
          dst[dstIndex] = static_cast<std::uint8_t>(std::clamp(
              static_cast<long>((sum + kernel.divisor / 2u) / kernel.divisor), 0L, 255L));
          continue;
        }

        float sum = 0.0f;
        for (std::size_t kernelIndex = 0; kernelIndex < kernel.weights.size(); ++kernelIndex) {
          const int sampleY = y + static_cast<int>(kernelIndex) - kernel.origin;
          if (sampleY < 0 || sampleY >= height) {
            continue;
          }

          const std::size_t srcIndex =
              static_cast<std::size_t>((sampleY * width + x) * 4 + channel);
          sum += kernel.weights[kernelIndex] * static_cast<float>(src[srcIndex]);
        }

        const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
        dst[dstIndex] = static_cast<std::uint8_t>(std::clamp(std::lround(sum), 0L, 255L));
      }
    }
  }
}

void applyGaussianBlur(tiny_skia::Pixmap& pixmap, double sigmaX, double sigmaY) {
  const int width = static_cast<int>(pixmap.width());
  const int height = static_cast<int>(pixmap.height());
  if (width <= 0 || height <= 0) {
    return;
  }

  std::vector<std::uint8_t> buffer(pixmap.data().begin(), pixmap.data().end());
  std::vector<std::uint8_t> scratch(buffer.size());

  if (sigmaX > 0.0) {
    const ConvolutionKernel kernel = makeBlurKernel(sigmaX);
    convolveHorizontal(buffer, scratch, width, height, kernel);
    buffer.swap(scratch);
  }

  if (sigmaY > 0.0) {
    const ConvolutionKernel kernel = makeBlurKernel(sigmaY);
    convolveVertical(buffer, scratch, width, height, kernel);
    buffer.swap(scratch);
  }

  std::copy(buffer.begin(), buffer.end(), pixmap.data().begin());
}

std::vector<std::uint8_t> premultiplyRgba(std::span<const std::uint8_t> rgbaPixels) {
  std::vector<std::uint8_t> result(rgbaPixels.begin(), rgbaPixels.end());
  for (std::size_t i = 0; i + 3 < result.size(); i += 4) {
    const unsigned alpha = result[i + 3];
    result[i + 0] =
        static_cast<std::uint8_t>((static_cast<unsigned>(result[i + 0]) * alpha + 127u) / 255u);
    result[i + 1] =
        static_cast<std::uint8_t>((static_cast<unsigned>(result[i + 1]) * alpha + 127u) / 255u);
    result[i + 2] =
        static_cast<std::uint8_t>((static_cast<unsigned>(result[i + 2]) * alpha + 127u) / 255u);
  }

  return result;
}

}  // namespace

RendererTinySkia::RendererTinySkia(bool verbose) : verbose_(verbose) {}

RendererTinySkia::~RendererTinySkia() {
#ifdef DONNER_TEXT_ENABLED
  delete static_cast<FontManager*>(fontManagerPtr_);
#endif
}
RendererTinySkia::RendererTinySkia(RendererTinySkia&&) noexcept = default;
RendererTinySkia& RendererTinySkia::operator=(RendererTinySkia&&) noexcept = default;

void RendererTinySkia::draw(SVGDocument& document) {
  RendererDriver driver(*this, verbose_);
  driver.draw(document);
}

void RendererTinySkia::beginFrame(const RenderViewport& viewport) {
  viewport_ = viewport;
  const int pixelWidth = static_cast<int>(viewport.size.x * viewport.devicePixelRatio);
  const int pixelHeight = static_cast<int>(viewport.size.y * viewport.devicePixelRatio);

  frame_ = createTransparentPixmap(pixelWidth, pixelHeight);
  currentTransform_ = Transformd();
  transformStack_.clear();
  currentClipMask_.reset();
  clipStack_.clear();
  surfaceStack_.clear();
  patternFillPaint_.reset();
  patternStrokePaint_.reset();
}

void RendererTinySkia::endFrame() {
  if (!surfaceStack_.empty() && verbose_) {
    std::cerr << "RendererTinySkia: unbalanced surface stack at endFrame\n";
  }

  surfaceStack_.clear();
  currentTransform_ = Transformd();
  transformStack_.clear();
  currentClipMask_.reset();
  clipStack_.clear();
}

void RendererTinySkia::setTransform(const Transformd& transform) {
  if (!surfaceStack_.empty() && surfaceStack_.back().kind == SurfaceKind::PatternTile) {
    const Transformd& rasterFromTile = surfaceStack_.back().patternRasterFromTile;
    currentTransform_ =
        scaleTransformOutput(transform, Vector2d(rasterFromTile.data[0], rasterFromTile.data[3]));
  } else {
    currentTransform_ = transform;
  }
}

void RendererTinySkia::pushTransform(const Transformd& transform) {
  transformStack_.push_back(currentTransform_);
  currentTransform_ = transform * currentTransform_;
}

void RendererTinySkia::popTransform() {
  if (transformStack_.empty()) {
    return;
  }

  currentTransform_ = transformStack_.back();
  transformStack_.pop_back();
}

void RendererTinySkia::pushClip(const ResolvedClip& clip) {
  clipStack_.push_back(currentClipMask_);

  std::optional<tiny_skia::Mask> clipMask = buildClipMask(clip);
  if (!clipMask.has_value()) {
    return;
  }

  if (currentClipMask_.has_value()) {
    intersectMaskInPlace(*clipMask, *currentClipMask_);
  }

  currentClipMask_ = std::move(clipMask);
}

void RendererTinySkia::popClip() {
  if (clipStack_.empty()) {
    currentClipMask_.reset();
    return;
  }

  currentClipMask_ = std::move(clipStack_.back());
  clipStack_.pop_back();
}

void RendererTinySkia::pushIsolatedLayer(double opacity) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::IsolatedLayer;
  frame.opacity = opacity;
  frame.pixmap = createTransparentPixmap(static_cast<int>(currentPixmap().width()),
                                         static_cast<int>(currentPixmap().height()));
  surfaceStack_.push_back(std::move(frame));
}

void RendererTinySkia::popIsolatedLayer() {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::IsolatedLayer) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();
  compositePixmap(frame.pixmap, frame.opacity);
}

void RendererTinySkia::pushFilterLayer(std::span<const FilterEffect> effects) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::FilterLayer;
  frame.effects.assign(effects.begin(), effects.end());
  frame.pixmap = createTransparentPixmap(static_cast<int>(currentPixmap().width()),
                                         static_cast<int>(currentPixmap().height()));
  surfaceStack_.push_back(std::move(frame));
}

void RendererTinySkia::popFilterLayer() {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::FilterLayer) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();
  applyFilters(frame.pixmap, frame.effects);
  compositePixmap(frame.pixmap, 1.0);
}

void RendererTinySkia::pushMask(const std::optional<Boxd>& maskBounds) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::MaskCapture;
  frame.maskBounds = maskBounds;
  frame.maskBoundsTransform = currentTransform_;
  frame.pixmap = createTransparentPixmap(static_cast<int>(currentPixmap().width()),
                                         static_cast<int>(currentPixmap().height()));
  surfaceStack_.push_back(std::move(frame));
}

void RendererTinySkia::transitionMaskToContent() {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::MaskCapture) {
    return;
  }

  SurfaceFrame& frame = surfaceStack_.back();
  frame.maskAlpha =
      tiny_skia::Mask::fromPixmap(frame.pixmap.view(), tiny_skia::MaskType::Luminance);

  if (frame.maskAlpha.has_value() && frame.maskBounds.has_value()) {
    std::optional<tiny_skia::Mask> boundsMask =
        createMaskForSize(frame.pixmap.width(), frame.pixmap.height());
    if (boundsMask.has_value()) {
      drawRectIntoMask(*boundsMask, *frame.maskBounds, frame.maskBoundsTransform, antialias_);
      intersectMaskInPlace(*frame.maskAlpha, *boundsMask);
    }
  }

  frame.kind = SurfaceKind::MaskContent;
  frame.pixmap = createTransparentPixmap(static_cast<int>(frame.pixmap.width()),
                                         static_cast<int>(frame.pixmap.height()));
}

void RendererTinySkia::popMask() {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::MaskContent) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();
  if (frame.maskAlpha.has_value()) {
    auto pixmapView = frame.pixmap.mutableView();
    tiny_skia::Painter::applyMask(pixmapView, *frame.maskAlpha);
  }
  compositePixmap(frame.pixmap, 1.0);
}

void RendererTinySkia::beginPatternTile(const Boxd& tileRect, const Transformd& patternToTarget) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::PatternTile;
  frame.savedTransform = currentTransform_;
  frame.savedTransformStack = transformStack_;
  frame.savedClipMask = currentClipMask_;
  frame.savedClipStack = clipStack_;
  const Transformd deviceFromPattern = frame.savedTransform * patternToTarget;
  const Vector2d requestedRasterScale = patternRasterScaleForTransform(deviceFromPattern);
  const int pixelWidth =
      std::max(1, static_cast<int>(std::ceil(tileRect.width() * requestedRasterScale.x)));
  const int pixelHeight =
      std::max(1, static_cast<int>(std::ceil(tileRect.height() * requestedRasterScale.y)));
  const Vector2d rasterScale =
      effectivePatternRasterScale(tileRect, pixelWidth, pixelHeight, requestedRasterScale);
  frame.patternRasterFromTile = Transformd::Scale(rasterScale);
  frame.patternToTarget = patternToTarget;
  frame.patternToTarget.data[4] *= rasterScale.x;
  frame.patternToTarget.data[5] *= rasterScale.y;
  frame.patternToTarget =
      frame.patternToTarget * Transformd::Scale(1.0 / rasterScale.x, 1.0 / rasterScale.y);
  frame.pixmap = createTransparentPixmap(pixelWidth, pixelHeight);

  surfaceStack_.push_back(std::move(frame));

  currentTransform_ = surfaceStack_.back().patternRasterFromTile;
  transformStack_.clear();
  currentClipMask_.reset();
  clipStack_.clear();
}

void RendererTinySkia::endPatternTile(bool forStroke) {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::PatternTile) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();

  currentTransform_ = frame.savedTransform;
  transformStack_ = std::move(frame.savedTransformStack);
  currentClipMask_ = std::move(frame.savedClipMask);
  clipStack_ = std::move(frame.savedClipStack);
  PatternPaintState state{std::move(frame.pixmap), frame.patternToTarget};
  if (forStroke) {
    patternStrokePaint_ = std::move(state);
  } else {
    patternFillPaint_ = std::move(state);
  }
}

void RendererTinySkia::setPaint(const PaintParams& paint) {
  paint_ = paint;
  paintOpacity_ = paint.opacity;
}

void RendererTinySkia::drawPath(const PathShape& path, const StrokeParams& stroke) {
  if (currentPixmap().width() == 0 || currentPixmap().height() == 0) {
    return;
  }

  const tiny_skia::Path tinyPath = toTinyPath(path.path);
  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;

  const bool usedPatternFill = patternFillPaint_.has_value();
  if (std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(path.path.bounds())) {
    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::fillPath(pixmapView, tinyPath, *fillPaint, toTinyFillRule(path.fillRule),
                                 toTinyTransform(currentTransform_), mask);
    if (usedPatternFill) {
      patternFillPaint_.reset();
    }
  }

  StrokeParams adjustedStroke = stroke;
  if (!adjustedStroke.dashArray.empty() && adjustedStroke.pathLength > 0.0 &&
      !NearZero(adjustedStroke.pathLength)) {
    const double actualLength = path.path.pathLength();
    const double dashUnitsScale = actualLength / adjustedStroke.pathLength;
    for (double& dash : adjustedStroke.dashArray) {
      dash *= dashUnitsScale;
    }
    adjustedStroke.dashOffset *= dashUnitsScale;
  }

  const bool usedPatternStroke = patternStrokePaint_.has_value();
  if (std::optional<tiny_skia::Paint> strokePaint =
          makeStrokePaint(path.path.bounds(), adjustedStroke)) {
    tiny_skia::Stroke tinyStroke;
    tinyStroke.width = NarrowToFloat(adjustedStroke.strokeWidth);
    tinyStroke.miterLimit = NarrowToFloat(adjustedStroke.miterLimit);
    tinyStroke.lineCap = toTinyLineCap(adjustedStroke.lineCap);
    tinyStroke.lineJoin = toTinyLineJoin(adjustedStroke.lineJoin);

    if (!adjustedStroke.dashArray.empty()) {
      const int repeats = (adjustedStroke.dashArray.size() & 1u) != 0u ? 2 : 1;
      std::vector<float> dashArray;
      dashArray.reserve(adjustedStroke.dashArray.size() * repeats);
      for (int i = 0; i < repeats; ++i) {
        for (double dash : adjustedStroke.dashArray) {
          dashArray.push_back(NarrowToFloat(dash));
        }
      }

      tinyStroke.dash = tiny_skia::StrokeDash::create(std::move(dashArray),
                                                      NarrowToFloat(adjustedStroke.dashOffset));
    }

    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::strokePath(pixmapView, tinyPath, *strokePaint, tinyStroke,
                                   toTinyTransform(currentTransform_), mask);
    if (usedPatternStroke) {
      patternStrokePaint_.reset();
    }
  }
}

void RendererTinySkia::drawRect(const Boxd& rect, const StrokeParams& stroke) {
  if (currentPixmap().width() == 0 || currentPixmap().height() == 0) {
    return;
  }

  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
  const std::optional<tiny_skia::Rect> tinyRect = toTinyRect(rect);
  if (!tinyRect.has_value()) {
    return;
  }

  const bool usedPatternFill = patternFillPaint_.has_value();
  if (std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(rect)) {
    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::fillRect(pixmapView, *tinyRect, *fillPaint,
                                 toTinyTransform(currentTransform_), mask);
    if (usedPatternFill) {
      patternFillPaint_.reset();
    }
  }

  const bool usedPatternStroke = patternStrokePaint_.has_value();
  if (std::optional<tiny_skia::Paint> strokePaint = makeStrokePaint(rect, stroke)) {
    const tiny_skia::Path path = tiny_skia::Path::fromRect(*tinyRect);
    tiny_skia::Stroke tinyStroke;
    tinyStroke.width = NarrowToFloat(stroke.strokeWidth);
    tinyStroke.miterLimit = NarrowToFloat(stroke.miterLimit);
    tinyStroke.lineCap = toTinyLineCap(stroke.lineCap);
    tinyStroke.lineJoin = toTinyLineJoin(stroke.lineJoin);

    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::strokePath(pixmapView, path, *strokePaint, tinyStroke,
                                   toTinyTransform(currentTransform_), mask);
    if (usedPatternStroke) {
      patternStrokePaint_.reset();
    }
  }
}

void RendererTinySkia::drawEllipse(const Boxd& bounds, const StrokeParams& stroke) {
  const std::optional<tiny_skia::Rect> oval = toTinyRect(bounds);
  if (!oval.has_value()) {
    return;
  }

  tiny_skia::PathBuilder builder;
  builder.pushOval(*oval);
  const tiny_skia::Path path = builder.finish().value_or(tiny_skia::Path());
  if (path.empty()) {
    return;
  }

  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
  const bool usedPatternFill = patternFillPaint_.has_value();
  if (std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(bounds)) {
    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::fillPath(pixmapView, path, *fillPaint, tiny_skia::FillRule::Winding,
                                 toTinyTransform(currentTransform_), mask);
    if (usedPatternFill) {
      patternFillPaint_.reset();
    }
  }

  const bool usedPatternStroke = patternStrokePaint_.has_value();
  if (std::optional<tiny_skia::Paint> strokePaint = makeStrokePaint(bounds, stroke)) {
    tiny_skia::Stroke tinyStroke;
    tinyStroke.width = NarrowToFloat(stroke.strokeWidth);
    tinyStroke.miterLimit = NarrowToFloat(stroke.miterLimit);
    tinyStroke.lineCap = toTinyLineCap(stroke.lineCap);
    tinyStroke.lineJoin = toTinyLineJoin(stroke.lineJoin);

    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::strokePath(pixmapView, path, *strokePaint, tinyStroke,
                                   toTinyTransform(currentTransform_), mask);
    if (usedPatternStroke) {
      patternStrokePaint_.reset();
    }
  }
}

void RendererTinySkia::drawImage(const ImageResource& image, const ImageParams& params) {
  if (image.data.empty() || image.width <= 0 || image.height <= 0) {
    return;
  }

  const std::optional<tiny_skia::Rect> targetRect = toTinyRect(params.targetRect);
  if (!targetRect.has_value()) {
    return;
  }

  std::vector<std::uint8_t> premultiplied = premultiplyRgba(image.data);
  auto maybePixmap = tiny_skia::Pixmap::fromVec(
      std::move(premultiplied), tiny_skia::IntSize(static_cast<std::uint32_t>(image.width),
                                                   static_cast<std::uint32_t>(image.height)));
  if (!maybePixmap.has_value()) {
    return;
  }

  const double scaleX = params.targetRect.width() / static_cast<double>(image.width);
  const double scaleY = params.targetRect.height() / static_cast<double>(image.height);
  const Transformd imageFromLocal = Transformd::Scale(scaleX, scaleY) *
                                    Transformd::Translate(params.targetRect.topLeft) *
                                    currentTransform_;

  tiny_skia::PixmapPaint paint;
  paint.opacity = NarrowToFloat(params.opacity * paintOpacity_);
  paint.blendMode = tiny_skia::BlendMode::SourceOver;
  paint.quality = params.imageRenderingPixelated ? tiny_skia::FilterQuality::Nearest
                                                 : tiny_skia::FilterQuality::Bilinear;
  paint.unpremulStore = surfaceStack_.empty();

  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
  auto pixmapView = currentPixmapView();
  tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, maybePixmap->view(), paint,
                                 toTinyTransform(imageFromLocal), mask);
}

void RendererTinySkia::drawText(const components::ComputedTextComponent& text,
                                const TextParams& params) {
#ifdef DONNER_TEXT_ENABLED
  if (currentPixmap().width() == 0 || currentPixmap().height() == 0) {
    return;
  }

  // Lazy-initialize the font manager.
  if (!fontManagerPtr_) {
    fontManagerPtr_ = new FontManager();
  }
  auto& fontManager = *static_cast<FontManager*>(fontManagerPtr_);

  // Register @font-face declarations so custom fonts can be resolved.
  // Only add new faces (faces_ grows monotonically, so track how many we've seen).
  const size_t existingFaces = fontManager.numFaces();
  if (params.fontFaces.size() > existingFaces) {
    for (size_t i = existingFaces; i < params.fontFaces.size(); ++i) {
      fontManager.addFontFace(params.fontFaces[i]);
    }
  }

#ifdef DONNER_TEXT_FULL
  TextShaper shaper(fontManager);
  std::vector<ShapedTextRun> runs = shaper.layout(text, params);
#else
  TextLayout layout(fontManager);
  std::vector<LayoutTextRun> runs = layout.layout(text, params);
#endif

  const stbtt_fontinfo* info = nullptr;
  float scale = 0.0f;
  const float fontSizePx = static_cast<float>(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));

  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;

  // Compute text bounding box from glyph positions for objectBoundingBox gradient mapping.
  // Per the SVG spec, the objectBoundingBox for text uses em-box cells defined by font metrics
  // (ascent above baseline, |descent| below baseline), not the raw font size.
  Boxd textBounds;
  {
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (size_t runIdx = 0; runIdx < runs.size(); ++runIdx) {
      const auto& run = runs[runIdx];

      // Resolve per-run font size (spans may override the text element's font size).
      float runFontSizePx = fontSizePx;
      if (runIdx < text.spans.size() && text.spans[runIdx].fontSize.value != 0.0) {
        runFontSizePx = static_cast<float>(text.spans[runIdx].fontSize.toPixels(
            params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
      }

      // Get font metrics for this run to compute proper em-box vertical extent.
      const stbtt_fontinfo* runInfo = run.font ? fontManager.fontInfo(run.font) : nullptr;
      float runScale = run.font ? fontManager.scaleForPixelHeight(run.font, runFontSizePx) : 0.0f;
      double emTop = static_cast<double>(runFontSizePx);  // fallback: full font size above baseline
      double emBottom = 0.0;                               // fallback: baseline
      if (runInfo && runScale > 0.0f) {
        int runAscent = 0;
        int runDescent = 0;
        int runLineGap = 0;
        stbtt_GetFontVMetrics(runInfo, &runAscent, &runDescent, &runLineGap);
        // ascent is positive (above baseline), descent is negative (below baseline).
        // In SVG's y-down space: top = baseline - ascent*scale, bottom = baseline - descent*scale.
        emTop = static_cast<double>(runAscent) * runScale;
        emBottom = -static_cast<double>(runDescent) * runScale;
      }

      for (const auto& glyph : run.glyphs) {
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

  // Use makeFillPaint/makeStrokePaint to support gradients, patterns, and solid colors.
  // These read from paint_ (set by setPaint()) which the driver already populated.
  std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(textBounds);
  const auto makeSolidFillPaint = [&](const css::Color& color, double spanOpacity = 1.0) {
    tiny_skia::Paint paint = makeBasePaint(antialias_);
    paint.unpremulStore = surfaceStack_.empty();

    css::RGBA rgba = color.rgba();
    rgba.a = static_cast<uint8_t>(std::round(static_cast<double>(rgba.a) *
                                             params.opacity * paintOpacity_ * spanOpacity));
    paint.shader = toTinyColor(rgba);
    return paint;
  };

  // Check if we have a stroke.
  const bool hasStroke = params.strokeParams.strokeWidth > 0.0;
  std::optional<tiny_skia::Paint> strokePaint;
  tiny_skia::Stroke tinyStroke;
  if (hasStroke) {
    strokePaint = makeStrokePaint(textBounds, params.strokeParams);
    tinyStroke.width = NarrowToFloat(params.strokeParams.strokeWidth);
    tinyStroke.miterLimit = NarrowToFloat(params.strokeParams.miterLimit);
    tinyStroke.lineCap = toTinyLineCap(params.strokeParams.lineCap);
    tinyStroke.lineJoin = toTinyLineJoin(params.strokeParams.lineJoin);
  }

  for (size_t runIndex = 0; runIndex < runs.size(); ++runIndex) {
    const auto& run = runs[runIndex];

    // Per-span font size: use the span's fontSize if set, otherwise the text element's.
    float spanFontSizePx = fontSizePx;
    if (runIndex < text.spans.size() && text.spans[runIndex].fontSize.value != 0.0) {
      spanFontSizePx = static_cast<float>(text.spans[runIndex].fontSize.toPixels(
          params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
    }

    if (run.font != FontHandle()) {
      info = fontManager.fontInfo(run.font);  // nullptr for bitmap-only fonts.
      scale = fontManager.scaleForPixelHeight(run.font, spanFontSizePx);
    }

    const bool isBitmapFont = run.font && fontManager.isBitmapOnly(run.font);
    if (!isBitmapFont && (!info || scale == 0.0f)) {
      continue;
    }

    std::optional<tiny_skia::Paint> spanFillPaint = fillPaint;
    if (runIndex < text.spans.size()) {
      const auto& span = text.spans[runIndex];
      const css::RGBA spanCurrentColor = paint_.currentColor.rgba();
      const float spanFillOpacity = NarrowToFloat(paint_.fillOpacity);

      if (const auto* solid =
              std::get_if<PaintServer::Solid>(&span.resolvedFill)) {
        // Per-span solid fill color.
        spanFillPaint = makeSolidFillPaint(
            css::Color(solid->color.resolve(spanCurrentColor, spanFillOpacity)), span.opacity);
      } else if (const auto* ref =
                     std::get_if<components::PaintResolvedReference>(&span.resolvedFill)) {
        // Per-span gradient/pattern fill. Uses the text element's bbox (textBounds)
        // for objectBoundingBox mapping, per SVG spec ("tspan doesn't have a bbox").
        const float combinedOpacity = spanFillOpacity * static_cast<float>(span.opacity);
        if (auto shader = instantiateGradientShader(*ref, textBounds, paint_.viewBox,
                                                     spanCurrentColor, combinedOpacity)) {
          tiny_skia::Paint paint = makeBasePaint(antialias_);
          paint.unpremulStore = surfaceStack_.empty();
          paint.shader = std::move(*shader);
          spanFillPaint = paint;
        } else if (ref->fallback.has_value()) {
          spanFillPaint = makeSolidFillPaint(
              css::Color(ref->fallback->resolve(spanCurrentColor, spanFillOpacity)), span.opacity);
        }
      } else if (span.opacity < 1.0 && spanFillPaint.has_value()) {
        // No explicit fill but has per-span opacity — re-apply with opacity.
        spanFillPaint = makeSolidFillPaint(params.fillColor, span.opacity);
      }
    }

    for (const auto& glyph : run.glyphs) {
      if (glyph.glyphIndex == 0) {
        continue;  // .notdef glyph, skip.
      }

#ifdef DONNER_TEXT_FULL
      PathSpline glyphPath;
      if (!isBitmapFont) {
        glyphPath = shaper.glyphOutline(run.font, glyph.glyphIndex,
                                       scale * glyph.fontSizeScale);
      }

      // For bitmap fonts (color emoji), extract and draw the bitmap directly.
      if (glyphPath.empty()) {
        auto bitmap = shaper.bitmapGlyph(run.font, glyph.glyphIndex, scale);
        if (bitmap) {
          // Premultiply alpha for correct blending.
          std::vector<uint8_t> premul = PremultiplyRgba(bitmap->rgbaPixels);
          auto maybePixmap = tiny_skia::Pixmap::fromVec(
              std::move(premul),
              tiny_skia::IntSize(static_cast<uint32_t>(bitmap->width),
                                 static_cast<uint32_t>(bitmap->height)));
          if (!maybePixmap.has_value()) {
            continue;
          }

          // Compute target rect in document space: position with bearing, scaled size.
          const double targetX = glyph.xPosition + bitmap->bearingX;
          const double targetY = glyph.yPosition - bitmap->bearingY;
          const double targetW = static_cast<double>(bitmap->width) * bitmap->scale;
          const double targetH = static_cast<double>(bitmap->height) * bitmap->scale;

          // Use the same transform pattern as drawImage: Scale * Translate * currentTransform_.
          const double imgScaleX = targetW / static_cast<double>(bitmap->width);
          const double imgScaleY = targetH / static_cast<double>(bitmap->height);
          const Transformd imageFromLocal =
              Transformd::Scale(imgScaleX, imgScaleY) *
              Transformd::Translate(Vector2d(targetX, targetY)) * currentTransform_;

          tiny_skia::PixmapPaint paint;
          paint.opacity = NarrowToFloat(paintOpacity_);
          paint.blendMode = tiny_skia::BlendMode::SourceOver;
          paint.quality = tiny_skia::FilterQuality::Bilinear;
          paint.unpremulStore = surfaceStack_.empty();

          const tiny_skia::Mask* mask =
              currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
          auto pixmapView = currentPixmapView();
          tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, maybePixmap->view(), paint,
                                         toTinyTransform(imageFromLocal), mask);
          continue;
        }
      }

      if (glyphPath.empty()) {
        continue;
      }
#else
      PathSpline glyphPath =
          glyphToPathSpline(info, glyph.glyphIndex, scale * glyph.fontSizeScale);
      if (glyphPath.empty()) {
        continue;
      }
#endif

      // Place glyph geometry in document space, then let the renderer's current transform map it
      // to device space. This avoids relying on composed affine semantics that differ from TinySkia's.
      Transformd glyphFromLocal = Transformd::Translate(glyph.xPosition, glyph.yPosition);
      if (glyph.rotateDegrees != 0.0) {
        glyphFromLocal = Transformd::Rotate(glyph.rotateDegrees *
                                            MathConstants<double>::kPi / 180.0) *
                         glyphFromLocal;
      }

      const tiny_skia::Path tinyPath = toTinyPath(transformPathSpline(glyphPath, glyphFromLocal));
      auto pixmapView = currentPixmapView();

      // Fill.
      if (spanFillPaint) {
        tiny_skia::Painter::fillPath(pixmapView, tinyPath, *spanFillPaint,
                                     tiny_skia::FillRule::Winding,
                                     toTinyTransform(currentTransform_), mask);
      }

      // Stroke.
      if (hasStroke && strokePaint) {
        tiny_skia::Painter::strokePath(pixmapView, tinyPath, *strokePaint, tinyStroke,
                                       toTinyTransform(currentTransform_), mask);
      }
    }

    // Draw text-decoration lines using font metrics.
    if (params.textDecoration != TextDecoration::None && !run.glyphs.empty() && info) {
      int ascent = 0;
      int descent = 0;
      int lineGap = 0;
      stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);

      // Read underline position/thickness from the font's 'post' table (offsets 8 and 10).
      // underlinePosition is negative in font units (below baseline).
      double fontUnderlinePos = 0.0;
      double fontUnderlineThick = 0.0;
      {
        // Locate the 'post' table in the TrueType table directory.
        const unsigned char* data = info->data;
        const int fontstart = info->fontstart;
        const int numTables = (data[fontstart + 4] << 8) | data[fontstart + 5];
        for (int i = 0; i < numTables; ++i) {
          const int tableDir = fontstart + 12 + i * 16;
          if (data[tableDir] == 'p' && data[tableDir + 1] == 'o' && data[tableDir + 2] == 's' &&
              data[tableDir + 3] == 't') {
            const unsigned int postOffset = static_cast<unsigned int>(data[tableDir + 8]) << 24 |
                                            static_cast<unsigned int>(data[tableDir + 9]) << 16 |
                                            static_cast<unsigned int>(data[tableDir + 10]) << 8 |
                                            static_cast<unsigned int>(data[tableDir + 11]);
            // post table: offset 8 = underlinePosition (int16), offset 10 = underlineThickness
            // (int16).
            fontUnderlinePos =
                static_cast<double>(static_cast<int16_t>((data[postOffset + 8] << 8) |
                                                         data[postOffset + 9]));
            fontUnderlineThick =
                static_cast<double>(static_cast<int16_t>((data[postOffset + 10] << 8) |
                                                         data[postOffset + 11]));
            break;
          }
        }
      }

      // Post table metrics are in font design units — scale by fontSize/unitsPerEm, not by
      // stbtt_ScaleForPixelHeight (which normalizes to ascent-descent height instead of em).
      const float emScale = stbtt_ScaleForMappingEmToPixels(info, fontSizePx);
      // Use font metrics for thickness, with heuristic fallback.
      const double thickness = fontUnderlineThick > 0.0
                                   ? fontUnderlineThick * emScale
                                   : static_cast<double>(ascent - descent) * scale / 18.0;

      // Compute vertical offset from baseline for the decoration type.
      // In our Y-down coordinate system, positive = below baseline.
      double decoOffsetY = 0.0;
      if (params.textDecoration == TextDecoration::Underline) {
        // fontUnderlinePos is typically negative (below baseline in font coords).
        // Negate it since we need positive = downward in our coord system.
        decoOffsetY = fontUnderlinePos != 0.0 ? -fontUnderlinePos * emScale
                                              : -static_cast<double>(descent) * scale * 0.4;
      } else if (params.textDecoration == TextDecoration::Overline) {
        decoOffsetY = -static_cast<double>(ascent) * scale;
      } else if (params.textDecoration == TextDecoration::LineThrough) {
        decoOffsetY = -static_cast<double>(ascent) * scale * 0.35;
      }

      // The post table's underlinePosition is the center of the stroke, so offset the
      // rectangle top edge by half the thickness to center the stroke on that position.
      const double decoTopY = decoOffsetY - thickness / 2.0;

      // Check if any glyph has rotation — if so, draw per-glyph decoration segments.
      const bool hasRotation = std::any_of(
          run.glyphs.begin(), run.glyphs.end(),
          [](const auto& g) { return g.rotateDegrees != 0.0; });

      if (hasRotation) {
        // Per-glyph decoration: each segment is a rectangle under the glyph, rotated with it.
        for (const auto& glyph : run.glyphs) {
          if (glyph.glyphIndex == 0 || glyph.xAdvance <= 0.0) {
            continue;
          }

          // Build a horizontal rectangle in glyph-local space (before rotation).
          PathSpline segPath;
          segPath.moveTo(Vector2d(0.0, decoTopY));
          segPath.lineTo(Vector2d(glyph.xAdvance, decoTopY));
          segPath.lineTo(Vector2d(glyph.xAdvance, decoTopY + thickness));
          segPath.lineTo(Vector2d(0.0, decoTopY + thickness));
          segPath.closePath();

          // Apply the same transform as the glyph: rotate then translate.
          Transformd segTransform = Transformd::Translate(glyph.xPosition, glyph.yPosition);
          if (glyph.rotateDegrees != 0.0) {
            segTransform = Transformd::Rotate(glyph.rotateDegrees *
                                              MathConstants<double>::kPi / 180.0) *
                           segTransform;
          }

          const tiny_skia::Path tinySegPath =
              toTinyPath(transformPathSpline(segPath, segTransform));
          auto pixmapView = currentPixmapView();

          if (spanFillPaint) {
            tiny_skia::Painter::fillPath(pixmapView, tinySegPath, *spanFillPaint,
                                         tiny_skia::FillRule::Winding,
                                         toTinyTransform(currentTransform_), mask);
          }
        }
      } else {
        // No rotation: draw a single horizontal decoration line across the run.
        const double firstX = run.glyphs.front().xPosition;
        const double lastX = run.glyphs.back().xPosition + run.glyphs.back().xAdvance;
        const double baseY = run.glyphs.front().yPosition;
        const double lineWidth = lastX - firstX;
        const double decoY = baseY + decoTopY;

        PathSpline decoPath;
        decoPath.moveTo(Vector2d(firstX, decoY));
        decoPath.lineTo(Vector2d(firstX + lineWidth, decoY));
        decoPath.lineTo(Vector2d(firstX + lineWidth, decoY + thickness));
        decoPath.lineTo(Vector2d(firstX, decoY + thickness));
        decoPath.closePath();

        const tiny_skia::Path tinyDecoPath = toTinyPath(decoPath);
        auto pixmapView = currentPixmapView();

        if (spanFillPaint) {
          tiny_skia::Painter::fillPath(pixmapView, tinyDecoPath, *spanFillPaint,
                                       tiny_skia::FillRule::Winding,
                                       toTinyTransform(currentTransform_), mask);
        }
      }
    }
  }

  // Consume pattern paints after use, matching drawPath/drawRect/drawEllipse behavior.
  if (patternFillPaint_.has_value()) {
    patternFillPaint_.reset();
  }
  if (patternStrokePaint_.has_value()) {
    patternStrokePaint_.reset();
  }
#else
  (void)text;
  (void)params;
  maybeWarnUnsupportedText();
#endif
}

RendererBitmap RendererTinySkia::takeSnapshot() const {
  RendererBitmap snapshot;
  snapshot.dimensions = Vector2i(width(), height());
  snapshot.rowBytes = static_cast<std::size_t>(width()) * 4u;
  if (frame_.width() == 0 || frame_.height() == 0) {
    return snapshot;
  }

  auto maybeCopy = tiny_skia::Pixmap::fromVec(
      std::vector<std::uint8_t>(frame_.data().begin(), frame_.data().end()), frame_.size());
  if (!maybeCopy.has_value()) {
    snapshot.dimensions = Vector2i::Zero();
    snapshot.rowBytes = 0;
    return snapshot;
  }

  snapshot.pixels = maybeCopy->release();
  return snapshot;
}

bool RendererTinySkia::save(const char* filename) {
  const RendererBitmap snapshot = takeSnapshot();
  if (snapshot.empty()) {
    return false;
  }

  return RendererImageIO::writeRgbaPixelsToPngFile(filename, snapshot.pixels, snapshot.dimensions.x,
                                                   snapshot.dimensions.y);
}

std::unique_ptr<RendererInterface> RendererTinySkia::createOffscreenInstance() const {
  return std::make_unique<RendererTinySkia>(verbose_);
}

int RendererTinySkia::width() const {
  return static_cast<int>(frame_.width());
}

int RendererTinySkia::height() const {
  return static_cast<int>(frame_.height());
}

tiny_skia::Pixmap& RendererTinySkia::currentPixmap() {
  return surfaceStack_.empty() ? frame_ : surfaceStack_.back().pixmap;
}

const tiny_skia::Pixmap& RendererTinySkia::currentPixmap() const {
  return surfaceStack_.empty() ? frame_ : surfaceStack_.back().pixmap;
}

tiny_skia::MutablePixmapView RendererTinySkia::currentPixmapView() {
  return currentPixmap().mutableView();
}

std::optional<tiny_skia::Mask> RendererTinySkia::buildClipMask(const ResolvedClip& clip) const {
  if (clip.empty()) {
    return std::nullopt;
  }

  if (verbose_) {
    std::cout << "[TinySkia::buildClipMask] clipRect=";
    if (clip.clipRect.has_value()) {
      std::cout << *clip.clipRect;
    } else {
      std::cout << "none";
    }
    std::cout << " clipPaths=" << clip.clipPaths.size()
              << "\n  currentTransform=" << currentTransform_
              << "  clipPathUnitsTransform=" << clip.clipPathUnitsTransform;
  }

  const auto createMask = [&]() {
    std::optional<tiny_skia::Mask> mask =
        createMaskForSize(currentPixmap().width(), currentPixmap().height());
    if (mask.has_value()) {
      std::fill(mask->data().begin(), mask->data().end(), 0);
    }
    return mask;
  };

  std::optional<tiny_skia::Mask> rectMask;
  if (clip.clipRect.has_value()) {
    rectMask = createMask();
    if (rectMask.has_value()) {
      drawRectIntoMask(*rectMask, *clip.clipRect, currentTransform_, antialias_);
    }
  }

  const auto renderShapeMask = [&](const PathShape& shape) -> std::optional<tiny_skia::Mask> {
    std::optional<tiny_skia::Mask> shapeMask = createMask();
    if (!shapeMask.has_value()) {
      return std::nullopt;
    }

    const tiny_skia::Path path = toTinyPath(shape.path);
    if (path.empty()) {
      if (verbose_) {
        std::cout << "\n  shape layer=" << shape.layer << " empty path";
      }
      return shapeMask;
    }

    const Transformd clipPathTransform =
        clip.clipPathUnitsTransform * shape.entityFromParent * currentTransform_;
    if (verbose_) {
      const Boxd pathBounds = shape.path.bounds();
      std::cout << "\n  shape layer=" << shape.layer << " bounds=" << pathBounds
                << "\n    entityFromParent=" << shape.entityFromParent
                << "    combinedTransform=" << clipPathTransform
                << "    transformedBounds=" << clipPathTransform.transformBox(pathBounds);
    }
    shapeMask->fillPath(path, toTinyFillRule(shape.fillRule), antialias_,
                        toTinyTransform(clipPathTransform));
    return shapeMask;
  };

  std::optional<tiny_skia::Mask> pathMask;
  if (!clip.clipPaths.empty()) {
    std::ptrdiff_t index = static_cast<std::ptrdiff_t>(clip.clipPaths.size()) - 1;

    std::function<std::optional<tiny_skia::Mask>(int)> buildLayerMask =
        [&](int layer) -> std::optional<tiny_skia::Mask> {
      std::optional<tiny_skia::Mask> layerMask;
      while (index >= 0 && clip.clipPaths[static_cast<std::size_t>(index)].layer == layer) {
        const PathShape& shape = clip.clipPaths[static_cast<std::size_t>(index)];
        std::optional<tiny_skia::Mask> shapeMask = renderShapeMask(shape);
        --index;

        if (shapeMask.has_value() && index >= 0 &&
            clip.clipPaths[static_cast<std::size_t>(index)].layer > layer) {
          std::optional<tiny_skia::Mask> nestedMask =
              buildLayerMask(clip.clipPaths[static_cast<std::size_t>(index)].layer);
          if (nestedMask.has_value()) {
            intersectMaskInPlace(*shapeMask, *nestedMask);
          }
        }

        if (!shapeMask.has_value()) {
          continue;
        }

        if (!layerMask.has_value()) {
          layerMask = std::move(shapeMask);
        } else {
          unionMaskInPlace(*layerMask, *shapeMask);
        }
      }
      return layerMask;
    };

    pathMask = buildLayerMask(clip.clipPaths.back().layer);
  }

  std::optional<tiny_skia::Mask> result;
  if (rectMask.has_value()) {
    result = std::move(rectMask);
  }
  if (pathMask.has_value()) {
    if (result.has_value()) {
      intersectMaskInPlace(*result, *pathMask);
    } else {
      result = std::move(pathMask);
    }
  }

  if (verbose_) {
    std::cout << "\n";
  }

  return result;
}

std::optional<tiny_skia::Paint> RendererTinySkia::makeFillPaint(const Boxd& bounds) {
  if (std::holds_alternative<PaintServer::None>(paint_.fill)) {
    return std::nullopt;
  }

  tiny_skia::Paint paint = makeBasePaint(antialias_);
  paint.unpremulStore = surfaceStack_.empty();

  if (patternFillPaint_.has_value()) {
    paint.shader =
        tiny_skia::Pattern(patternFillPaint_->pixmap.view(), tiny_skia::SpreadMode::Repeat,
                           tiny_skia::FilterQuality::Bilinear, NarrowToFloat(paint_.fillOpacity),
                           toTinyTransform(patternFillPaint_->patternToTarget));
    return paint;
  }

  const css::RGBA currentColor = paint_.currentColor.rgba();
  const float fillOpacity = NarrowToFloat(paint_.fillOpacity);

  if (const auto* solid = std::get_if<PaintServer::Solid>(&paint_.fill)) {
    paint.shader = toTinyColor(solid->color.resolve(currentColor, fillOpacity));
    return paint;
  }

  if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint_.fill)) {
    if (std::optional<tiny_skia::Shader> shader =
            instantiateGradientShader(*ref, bounds, paint_.viewBox, currentColor, fillOpacity)) {
      paint.shader = std::move(*shader);
      return paint;
    }

    if (ref->fallback.has_value()) {
      paint.shader = toTinyColor(ref->fallback->resolve(currentColor, fillOpacity));
      return paint;
    }
  }

  return std::nullopt;
}

std::optional<tiny_skia::Paint> RendererTinySkia::makeStrokePaint(const Boxd& bounds,
                                                                  const StrokeParams& stroke) {
  if (std::holds_alternative<PaintServer::None>(paint_.stroke) || stroke.strokeWidth <= 0.0) {
    return std::nullopt;
  }

  tiny_skia::Paint paint = makeBasePaint(antialias_);
  paint.unpremulStore = surfaceStack_.empty();

  if (patternStrokePaint_.has_value()) {
    paint.shader =
        tiny_skia::Pattern(patternStrokePaint_->pixmap.view(), tiny_skia::SpreadMode::Repeat,
                           tiny_skia::FilterQuality::Bilinear, NarrowToFloat(paint_.strokeOpacity),
                           toTinyTransform(patternStrokePaint_->patternToTarget));
    return paint;
  }

  const css::RGBA currentColor = paint_.currentColor.rgba();
  const float strokeOpacity = NarrowToFloat(paint_.strokeOpacity);

  if (const auto* solid = std::get_if<PaintServer::Solid>(&paint_.stroke)) {
    paint.shader = toTinyColor(solid->color.resolve(currentColor, strokeOpacity));
    return paint;
  }

  if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint_.stroke)) {
    if (std::optional<tiny_skia::Shader> shader =
            instantiateGradientShader(*ref, bounds, paint_.viewBox, currentColor, strokeOpacity)) {
      paint.shader = std::move(*shader);
      return paint;
    }

    if (ref->fallback.has_value()) {
      paint.shader = toTinyColor(ref->fallback->resolve(currentColor, strokeOpacity));
      return paint;
    }
  }

  return std::nullopt;
}

tiny_skia::Pixmap RendererTinySkia::createTransparentPixmap(int width, int height) const {
  if (width <= 0 || height <= 0) {
    return tiny_skia::Pixmap();
  }

  auto maybePixmap = tiny_skia::Pixmap::fromSize(static_cast<std::uint32_t>(width),
                                                 static_cast<std::uint32_t>(height));
  if (!maybePixmap.has_value()) {
    return tiny_skia::Pixmap();
  }

  maybePixmap->fill(tiny_skia::Color::transparent);
  return std::move(*maybePixmap);
}

void RendererTinySkia::compositePixmap(const tiny_skia::Pixmap& pixmap, double opacity) {
  if (opacity <= 0.0 || pixmap.width() == 0 || pixmap.height() == 0) {
    return;
  }

  tiny_skia::PixmapPaint paint;
  paint.opacity = NarrowToFloat(opacity);
  paint.blendMode = tiny_skia::BlendMode::SourceOver;
  paint.quality = tiny_skia::FilterQuality::Nearest;
  paint.unpremulStore = surfaceStack_.empty();

  auto pixmapView = currentPixmapView();
  tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, pixmap.view(), paint);
}

void RendererTinySkia::applyFilters(tiny_skia::Pixmap& pixmap,
                                    std::span<const FilterEffect> effects) {
  for (const FilterEffect& effect : effects) {
    std::visit(
        [&](const auto& resolvedEffect) {
          using T = std::decay_t<decltype(resolvedEffect)>;
          if constexpr (std::is_same_v<T, FilterEffect::Blur>) {
            applyGaussianBlur(pixmap, resolvedEffect.stdDeviationX.value,
                              resolvedEffect.stdDeviationY.value);
          } else if constexpr (!std::is_same_v<T, FilterEffect::None>) {
            maybeWarnUnsupportedFilter(effect);
          }
        },
        effect.value);
  }
}

void RendererTinySkia::maybeWarnUnsupportedFilter(const FilterEffect& effect) {
  (void)effect;
  if (!verbose_ || warnedUnsupportedFilter_) {
    return;
  }

  warnedUnsupportedFilter_ = true;
  std::cerr << "RendererTinySkia: some filter effects are not implemented\n";
}

void RendererTinySkia::maybeWarnUnsupportedText() {
  if (!verbose_ || warnedUnsupportedText_) {
    return;
  }

  warnedUnsupportedText_ = true;
  std::cerr << "RendererTinySkia: text rendering is not implemented\n";
}

}  // namespace donner::svg
