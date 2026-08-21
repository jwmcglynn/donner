#include "donner/svg/renderer/FilterGraphExecutor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "donner/svg/renderer/ImageSampling.h"
#include "donner/svg/renderer/PixelFormatUtils.h"
#include "tiny_skia/filter/FilterGraph.h"

namespace donner::svg {

namespace {

double BoundedPositiveFilterPixels(double value) {
  if (!std::isfinite(value) || value <= 0.0) {
    return 0.0;
  }
  return std::min(value, static_cast<double>(components::kMaximumFilterPixelRadius));
}

int BoundedRoundedFilterPixels(double value, int magnitudeLimit) {
  if (!std::isfinite(value)) {
    return 0;
  }
  const double bounded =
      std::clamp(value, -static_cast<double>(magnitudeLimit), static_cast<double>(magnitudeLimit));
  return static_cast<int>(std::round(bounded));
}

double BoundedPixelEdge(double value, double minimum, double maximum) {
  if (std::isnan(value)) {
    return minimum;
  }
  return std::clamp(value, minimum, maximum);
}

tiny_skia::filter::PixelRect BoundedPixelRect(const Box2d& box, int width, int height) {
  const double minimum = -static_cast<double>(components::kMaximumFilterPixelOffset);
  const double maximumX = static_cast<double>(width + components::kMaximumFilterPixelOffset);
  const double maximumY = static_cast<double>(height + components::kMaximumFilterPixelOffset);
  const double x0 = BoundedPixelEdge(box.topLeft.x, minimum, maximumX);
  const double y0 = BoundedPixelEdge(box.topLeft.y, minimum, maximumY);
  const double x1 = BoundedPixelEdge(box.bottomRight.x, minimum, maximumX);
  const double y1 = BoundedPixelEdge(box.bottomRight.y, minimum, maximumY);
  return tiny_skia::filter::PixelRect{x0, y0, std::max(0.0, x1 - x0), std::max(0.0, y1 - y0)};
}

namespace fp = components::filter_primitive;
namespace gp = tiny_skia::filter::graph_primitive;

struct PremulRGBA {
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
  std::uint8_t a;
};

PremulRGBA FloodToPremul(const css::Color& color, double opacity) {
  const css::RGBA rgba = color.asRGBA();
  const double alpha = (rgba.a / 255.0) * opacity;
  return {static_cast<std::uint8_t>(std::round(rgba.r * alpha)),
          static_cast<std::uint8_t>(std::round(rgba.g * alpha)),
          static_cast<std::uint8_t>(std::round(rgba.b * alpha)),
          static_cast<std::uint8_t>(std::round(alpha * 255.0))};
}

tiny_skia::filter::NodeInput ConvertInput(const components::FilterInput& input) {
  return std::visit(
      [](const auto& value) -> tiny_skia::filter::NodeInput {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, components::FilterInput::Previous>) {
          return tiny_skia::filter::NodeInput();
        } else if constexpr (std::is_same_v<T, components::FilterStandardInput>) {
          return tiny_skia::filter::NodeInput(static_cast<tiny_skia::filter::StandardInput>(value));
        } else if constexpr (std::is_same_v<T, components::FilterInput::Named>) {
          return tiny_skia::filter::NodeInput(
              tiny_skia::filter::NodeInput::Named{std::string(value.name)});
        } else {
          return tiny_skia::filter::NodeInput();
        }
      },
      input.value);
}

double ComputePixelScale(const Transform2d& deviceFromFilter) {
  const Vector2d transformedXAxis = deviceFromFilter.transformPosition(Vector2d(1, 0)) -
                                    deviceFromFilter.transformPosition(Vector2d(0, 0));
  const Vector2d transformedYAxis = deviceFromFilter.transformPosition(Vector2d(0, 1)) -
                                    deviceFromFilter.transformPosition(Vector2d(0, 0));
  return std::sqrt(
      (transformedXAxis.x * transformedXAxis.x + transformedXAxis.y * transformedXAxis.y +
       transformedYAxis.x * transformedYAxis.x + transformedYAxis.y * transformedYAxis.y) /
      2.0);
}

std::array<double, 6> ComputePixelToUser(const Transform2d& deviceFromFilter) {
  const Transform2d filterFromDevice = deviceFromFilter.inverse();
  return {filterFromDevice.data[0], filterFromDevice.data[2], filterFromDevice.data[4],
          filterFromDevice.data[1], filterFromDevice.data[3], filterFromDevice.data[5]};
}

bool HasShear(const Transform2d& deviceFromFilter) {
  const double a = deviceFromFilter.data[0];
  const double b = deviceFromFilter.data[1];
  const double c = deviceFromFilter.data[2];
  const double d = deviceFromFilter.data[3];
  const double dot = a * c + b * d;
  const double firstAxisLengthSquared = a * a + b * b;
  const double secondAxisLengthSquared = c * c + d * d;
  return dot * dot > 0.0003 * firstAxisLengthSquared * secondAxisLengthSquared;
}

Box2d ComputePrimitiveUnitsBounds(const components::FilterGraph& filterGraph,
                                  bool usesObjectBoundingBox, double scaleX, double scaleY,
                                  int outputWidth, int outputHeight) {
  if (usesObjectBoundingBox) {
    return *filterGraph.elementBoundingBox;
  }

  const double userWidth =
      NearZero(filterGraph.userToPixelScale.x, 1e-12)
          ? (NearZero(scaleX, 1e-12) ? static_cast<double>(outputWidth)
                                     : static_cast<double>(outputWidth) / scaleX)
          : static_cast<double>(outputWidth) / filterGraph.userToPixelScale.x;
  const double userHeight =
      NearZero(filterGraph.userToPixelScale.y, 1e-12)
          ? (NearZero(scaleY, 1e-12) ? static_cast<double>(outputHeight)
                                     : static_cast<double>(outputHeight) / scaleY)
          : static_cast<double>(outputHeight) / filterGraph.userToPixelScale.y;
  return Box2d::FromXYWH(0.0, 0.0, userWidth, userHeight);
}

struct FilterConversionContext {
  FilterConversionContext(const components::FilterGraph& filterGraph,
                          const Transform2d& deviceFromFilter,
                          const std::optional<Box2d>& filterRegion, int outputWidth,
                          int outputHeight)
      : filterGraph(filterGraph),
        deviceFromFilter(deviceFromFilter),
        filterRegion(filterRegion),
        outputWidth(outputWidth),
        outputHeight(outputHeight),
        usesObjectBoundingBox(filterGraph.primitiveUnits == PrimitiveUnits::ObjectBoundingBox &&
                              filterGraph.elementBoundingBox.has_value()),
        boundingBoxWidth(usesObjectBoundingBox ? filterGraph.elementBoundingBox->width() : 1.0),
        boundingBoxHeight(usesObjectBoundingBox ? filterGraph.elementBoundingBox->height() : 1.0),
        boundingBoxX(usesObjectBoundingBox ? filterGraph.elementBoundingBox->topLeft.x : 0.0),
        boundingBoxY(usesObjectBoundingBox ? filterGraph.elementBoundingBox->topLeft.y : 0.0),
        scaleX(deviceFromFilter.transformVector(Vector2d(1.0, 0.0)).length()),
        scaleY(NearZero(scaleX, 1e-12) ? std::abs(deviceFromFilter.data[3])
                                       : std::abs(deviceFromFilter.determinant()) / scaleX),
        primitiveUnitsBounds(ComputePrimitiveUnitsBounds(filterGraph, usesObjectBoundingBox, scaleX,
                                                         scaleY, outputWidth, outputHeight)),
        pixelScale(ComputePixelScale(deviceFromFilter)),
        pixelToUser(ComputePixelToUser(deviceFromFilter)),
        hasShear(HasShear(deviceFromFilter)) {}

  double primitiveToPixelX(double value) const {
    return std::abs(usesObjectBoundingBox ? value * boundingBoxWidth : value) * scaleX;
  }

  double primitiveToPixelY(double value) const {
    return std::abs(usesObjectBoundingBox ? value * boundingBoxHeight : value) * scaleY;
  }

  Vector2d primitiveToPixelOffset(double dx, double dy) const {
    const Vector2d userOffset = usesObjectBoundingBox
                                    ? Vector2d(dx * boundingBoxWidth, dy * boundingBoxHeight)
                                    : Vector2d(dx, dy);
    return deviceFromFilter.transformVector(userOffset);
  }

  double resolvePrimitivePosition(const Lengthd& length, Lengthd::Extent extent) const {
    const double boundingBoxOrigin = extent == Lengthd::Extent::X ? boundingBoxX : boundingBoxY;
    const double boundingBoxDimension =
        extent == Lengthd::Extent::X ? boundingBoxWidth : boundingBoxHeight;
    if (usesObjectBoundingBox && length.unit == Lengthd::Unit::None) {
      return boundingBoxOrigin + length.value * boundingBoxDimension;
    }
    if (length.unit == Lengthd::Unit::Percent) {
      const double referenceOrigin =
          usesObjectBoundingBox ? boundingBoxOrigin
                                : (extent == Lengthd::Extent::X ? primitiveUnitsBounds.topLeft.x
                                                                : primitiveUnitsBounds.topLeft.y);
      const double referenceSize =
          usesObjectBoundingBox ? boundingBoxDimension
                                : (extent == Lengthd::Extent::X ? primitiveUnitsBounds.width()
                                                                : primitiveUnitsBounds.height());
      return referenceOrigin + referenceSize * length.value / 100.0;
    }
    return length.toPixels(primitiveUnitsBounds, FontMetrics(), extent);
  }

  double resolvePrimitiveSize(const Lengthd& length, Lengthd::Extent extent) const {
    const double boundingBoxDimension =
        extent == Lengthd::Extent::X ? boundingBoxWidth : boundingBoxHeight;
    if (usesObjectBoundingBox && length.unit == Lengthd::Unit::None) {
      return length.value * boundingBoxDimension;
    }
    if (length.unit == Lengthd::Unit::Percent) {
      const double referenceSize =
          usesObjectBoundingBox ? boundingBoxDimension
                                : (extent == Lengthd::Extent::X ? primitiveUnitsBounds.width()
                                                                : primitiveUnitsBounds.height());
      return referenceSize * length.value / 100.0;
    }
    return length.toPixels(primitiveUnitsBounds, FontMetrics(), extent);
  }

  tiny_skia::filter::LightSourceParams convertLightSource(const fp::LightSource& light) const {
    tiny_skia::filter::LightSourceParams result;
    result.type = static_cast<tiny_skia::filter::LightType>(light.type);
    result.azimuth = light.azimuth;
    result.elevation = light.elevation;

    const double userX =
        usesObjectBoundingBox ? light.x * boundingBoxWidth + boundingBoxX : light.x;
    const double userY =
        usesObjectBoundingBox ? light.y * boundingBoxHeight + boundingBoxY : light.y;
    const double userZ = usesObjectBoundingBox ? light.z * boundingBoxHeight : light.z;
    const double userPointsAtX =
        usesObjectBoundingBox ? light.pointsAtX * boundingBoxWidth + boundingBoxX : light.pointsAtX;
    const double userPointsAtY = usesObjectBoundingBox
                                     ? light.pointsAtY * boundingBoxHeight + boundingBoxY
                                     : light.pointsAtY;
    const double userPointsAtZ =
        usesObjectBoundingBox ? light.pointsAtZ * boundingBoxHeight : light.pointsAtZ;

    const Vector2d lightPixel = deviceFromFilter.transformPosition(Vector2d(userX, userY));
    const Vector2d pointsAtPixel =
        deviceFromFilter.transformPosition(Vector2d(userPointsAtX, userPointsAtY));
    result.x = lightPixel.x;
    result.y = lightPixel.y;
    result.z = userZ * pixelScale;
    result.pointsAtX = pointsAtPixel.x;
    result.pointsAtY = pointsAtPixel.y;
    result.pointsAtZ = userPointsAtZ * pixelScale;
    result.spotExponent = light.spotExponent;
    result.limitingConeAngle = light.limitingConeAngle;
    result.userX = userX;
    result.userY = userY;
    result.userZ = userZ;
    result.userPointsAtX = userPointsAtX;
    result.userPointsAtY = userPointsAtY;
    result.userPointsAtZ = userPointsAtZ;
    return result;
  }

  const components::FilterGraph& filterGraph;
  const Transform2d& deviceFromFilter;
  const std::optional<Box2d>& filterRegion;
  int outputWidth;
  int outputHeight;
  bool usesObjectBoundingBox;
  double boundingBoxWidth;
  double boundingBoxHeight;
  double boundingBoxX;
  double boundingBoxY;
  double scaleX;
  double scaleY;
  Box2d primitiveUnitsBounds;
  double pixelScale;
  std::array<double, 6> pixelToUser;
  bool hasShear;
};

struct PrimitiveConversionState {
  const FilterConversionContext& context;
  const tiny_skia::filter::FilterGraph& graph;
  const tiny_skia::filter::GraphNode& node;
};

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::GaussianBlur& primitive,
                                                   const PrimitiveConversionState& state) {
  gp::GaussianBlur blur;
  blur.sigmaX =
      primitive.stdDeviationX >= 0
          ? BoundedPositiveFilterPixels(state.context.primitiveToPixelX(primitive.stdDeviationX))
          : 0.0;
  blur.sigmaY =
      primitive.stdDeviationY >= 0
          ? BoundedPositiveFilterPixels(state.context.primitiveToPixelY(primitive.stdDeviationY))
          : 0.0;
  blur.edgeMode = static_cast<tiny_skia::filter::BlurEdgeMode>(primitive.edgeMode);
  return blur;
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::Flood& primitive,
                                                   const PrimitiveConversionState&) {
  const PremulRGBA color = FloodToPremul(primitive.floodColor, primitive.floodOpacity);
  return gp::Flood{color.r, color.g, color.b, color.a};
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::Offset& primitive,
                                                   const PrimitiveConversionState& state) {
  const Vector2d offset = state.context.primitiveToPixelOffset(primitive.dx, primitive.dy);
  return gp::Offset{
      BoundedRoundedFilterPixels(offset.x, components::kMaximumFilterPixelOffset),
      BoundedRoundedFilterPixels(offset.y, components::kMaximumFilterPixelOffset),
  };
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::Composite& primitive,
                                                   const PrimitiveConversionState&) {
  gp::Composite composite;
  composite.op = static_cast<tiny_skia::filter::CompositeOp>(primitive.op);
  composite.k1 = primitive.k1;
  composite.k2 = primitive.k2;
  composite.k3 = primitive.k3;
  composite.k4 = primitive.k4;
  return composite;
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::Blend& primitive,
                                                   const PrimitiveConversionState&) {
  gp::Blend blend;
  blend.mode = static_cast<tiny_skia::filter::BlendMode>(primitive.mode);
  return blend;
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::Merge&,
                                                   const PrimitiveConversionState&) {
  return gp::Merge{};
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::ColorMatrix& primitive,
                                                   const PrimitiveConversionState&) {
  gp::ColorMatrix colorMatrix;
  if (primitive.type == fp::ColorMatrix::Type::Matrix) {
    if (primitive.values.size() == 20) {
      for (std::size_t i = 0; i < 20; ++i) {
        colorMatrix.matrix[i] = primitive.values[i];
      }
    } else {
      colorMatrix.matrix = tiny_skia::filter::identityMatrix();
    }
  } else if (primitive.type == fp::ColorMatrix::Type::Saturate) {
    const double saturation = primitive.values.empty() ? 1.0 : primitive.values[0];
    colorMatrix.matrix = tiny_skia::filter::saturateMatrix(saturation);
  } else if (primitive.type == fp::ColorMatrix::Type::HueRotate) {
    const double angle = primitive.values.empty() ? 0.0 : primitive.values[0];
    colorMatrix.matrix = tiny_skia::filter::hueRotateMatrix(angle);
  } else if (primitive.type == fp::ColorMatrix::Type::LuminanceToAlpha) {
    colorMatrix.matrix = tiny_skia::filter::luminanceToAlphaMatrix();
  } else {
    colorMatrix.matrix = tiny_skia::filter::identityMatrix();
  }
  return colorMatrix;
}

gp::ComponentTransfer::Func ConvertTransferFunction(const fp::ComponentTransfer::Func& function) {
  gp::ComponentTransfer::Func result;
  result.type = static_cast<tiny_skia::filter::TransferFuncType>(function.type);
  result.tableValues = function.tableValues;
  result.slope = function.slope;
  result.intercept = function.intercept;
  result.amplitude = function.amplitude;
  result.exponent = function.exponent;
  result.offset = function.offset;
  return result;
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::ComponentTransfer& primitive,
                                                   const PrimitiveConversionState&) {
  gp::ComponentTransfer componentTransfer;
  componentTransfer.funcR = ConvertTransferFunction(primitive.funcR);
  componentTransfer.funcG = ConvertTransferFunction(primitive.funcG);
  componentTransfer.funcB = ConvertTransferFunction(primitive.funcB);
  componentTransfer.funcA = ConvertTransferFunction(primitive.funcA);
  return componentTransfer;
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::DropShadow& primitive,
                                                   const PrimitiveConversionState& state) {
  const PremulRGBA color = FloodToPremul(primitive.floodColor, primitive.floodOpacity);
  const Vector2d offset = state.context.primitiveToPixelOffset(primitive.dx, primitive.dy);
  gp::DropShadow dropShadow;
  dropShadow.r = color.r;
  dropShadow.g = color.g;
  dropShadow.b = color.b;
  dropShadow.a = color.a;
  dropShadow.dx = BoundedRoundedFilterPixels(offset.x, components::kMaximumFilterPixelOffset);
  dropShadow.dy = BoundedRoundedFilterPixels(offset.y, components::kMaximumFilterPixelOffset);
  dropShadow.sigmaX =
      primitive.stdDeviationX >= 0
          ? BoundedPositiveFilterPixels(state.context.primitiveToPixelX(primitive.stdDeviationX))
          : 0.0;
  dropShadow.sigmaY =
      primitive.stdDeviationY >= 0
          ? BoundedPositiveFilterPixels(state.context.primitiveToPixelY(primitive.stdDeviationY))
          : 0.0;
  return dropShadow;
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::Morphology& primitive,
                                                   const PrimitiveConversionState& state) {
  gp::Morphology morphology;
  if (primitive.radiusX < 0 || primitive.radiusY < 0 ||
      (primitive.radiusX == 0 && primitive.radiusY == 0)) {
    morphology.radiusX = 0;
    morphology.radiusY = 0;
  } else {
    morphology.op = primitive.op == fp::Morphology::Operator::Erode
                        ? tiny_skia::filter::MorphologyOp::Erode
                        : tiny_skia::filter::MorphologyOp::Dilate;
    morphology.radiusX = BoundedRoundedFilterPixels(
        state.context.primitiveToPixelX(primitive.radiusX), components::kMaximumFilterPixelRadius);
    morphology.radiusY = BoundedRoundedFilterPixels(
        state.context.primitiveToPixelY(primitive.radiusY), components::kMaximumFilterPixelRadius);
  }
  return morphology;
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::ConvolveMatrix& primitive,
                                                   const PrimitiveConversionState&) {
  gp::ConvolveMatrix convolveMatrix;
  convolveMatrix.orderX = primitive.orderX;
  convolveMatrix.orderY = primitive.orderY;
  convolveMatrix.kernel = primitive.kernelMatrix;
  convolveMatrix.bias = primitive.bias;
  convolveMatrix.edgeMode = static_cast<tiny_skia::filter::ConvolveEdgeMode>(primitive.edgeMode);
  convolveMatrix.preserveAlpha = primitive.preserveAlpha;
  convolveMatrix.targetX = primitive.targetX.value_or(primitive.orderX / 2);
  convolveMatrix.targetY = primitive.targetY.value_or(primitive.orderY / 2);

  if (primitive.divisor.has_value()) {
    convolveMatrix.divisor = *primitive.divisor;
  } else {
    const int requiredSize = primitive.orderX * primitive.orderY;
    double sum = 0.0;
    for (std::size_t i = 0;
         i < primitive.kernelMatrix.size() && i < static_cast<std::size_t>(requiredSize); ++i) {
      sum += primitive.kernelMatrix[i];
    }
    convolveMatrix.divisor = std::abs(sum) < 1e-10 ? 1.0 : sum;
  }
  return convolveMatrix;
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::Tile&,
                                                   const PrimitiveConversionState&) {
  return gp::Tile{};
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::Turbulence& primitive,
                                                   const PrimitiveConversionState& state) {
  gp::Turbulence turbulence;
  turbulence.params.type = primitive.type == fp::Turbulence::Type::FractalNoise
                               ? tiny_skia::filter::TurbulenceType::FractalNoise
                               : tiny_skia::filter::TurbulenceType::Turbulence;
  turbulence.params.baseFrequencyX = primitive.baseFrequencyX;
  turbulence.params.baseFrequencyY = primitive.baseFrequencyY;
  turbulence.params.numOctaves = primitive.numOctaves;
  turbulence.params.seed = primitive.seed;
  turbulence.params.stitchTiles = primitive.stitchTiles;
  turbulence.params.tileWidth = state.context.outputWidth;
  turbulence.params.tileHeight = state.context.outputHeight;

  const double a = state.context.deviceFromFilter.data[0];
  const double b = state.context.deviceFromFilter.data[1];
  const double c = state.context.deviceFromFilter.data[2];
  const double d = state.context.deviceFromFilter.data[3];
  double determinant = a * d - b * c;
  if (std::abs(determinant) < 1e-10) {
    determinant = 1.0;
  }
  const double inverseDeterminant = 1.0 / determinant;
  turbulence.params.filterFromDeviceA = d * inverseDeterminant;
  turbulence.params.filterFromDeviceB = -c * inverseDeterminant;
  turbulence.params.filterFromDeviceC = -b * inverseDeterminant;
  turbulence.params.filterFromDeviceD = a * inverseDeterminant;
  return turbulence;
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::DisplacementMap& primitive,
                                                   const PrimitiveConversionState& state) {
  gp::DisplacementMap displacementMap;
  displacementMap.scale = state.context.usesObjectBoundingBox
                              ? primitive.scale * std::sqrt(state.context.boundingBoxWidth *
                                                            state.context.boundingBoxHeight)
                              : primitive.scale;
  displacementMap.xChannel =
      static_cast<tiny_skia::filter::DisplacementChannel>(primitive.xChannelSelector);
  displacementMap.yChannel =
      static_cast<tiny_skia::filter::DisplacementChannel>(primitive.yChannelSelector);
  return displacementMap;
}

gp::Image::Sampling ConvertImageSampling(ImageRendering imageRendering) {
  switch (imageRendering) {
    case ImageRendering::CrispEdges:
    case ImageRendering::OptimizeSpeed: return gp::Image::Sampling::CrispEdges;
    case ImageRendering::Pixelated: return gp::Image::Sampling::Pixelated;
    case ImageRendering::Auto:
    case ImageRendering::Smooth:
    case ImageRendering::HighQuality:
    case ImageRendering::OptimizeQuality: return gp::Image::Sampling::Smooth;
  }
  return gp::Image::Sampling::Smooth;
}

void PopulateTransformedFragmentImage(gp::Image& image, const fp::Image& primitive,
                                      const PrimitiveConversionState& state,
                                      std::span<const std::uint8_t> premultiplied) {
  const Transform2d viewBoxScaleInv =
      Transform2d::Scale(NearZero(state.context.filterGraph.userToPixelScale.x, 1e-12)
                             ? 1.0
                             : 1.0 / state.context.filterGraph.userToPixelScale.x,
                         NearZero(state.context.filterGraph.userToPixelScale.y, 1e-12)
                             ? 1.0
                             : 1.0 / state.context.filterGraph.userToPixelScale.y);
  const Transform2d regionOffset =
      Transform2d::Translate(primitive.fragmentRegionTopLeft.x, primitive.fragmentRegionTopLeft.y);
  const Transform2d deviceFromFragment =
      viewBoxScaleInv * regionOffset * state.context.deviceFromFilter;

  image.pixels = RasterizeImagePremultiplied(
      premultiplied, primitive.imageWidth, primitive.imageHeight, deviceFromFragment,
      state.context.outputWidth, state.context.outputHeight, primitive.imageRendering);
  image.width = state.context.outputWidth;
  image.height = state.context.outputHeight;
  image.targetRect =
      tiny_skia::filter::PixelRect{0.0, 0.0, static_cast<double>(state.context.outputWidth),
                                   static_cast<double>(state.context.outputHeight)};
}

void PopulateUntransformedFragmentImage(gp::Image& image, const fp::Image& primitive,
                                        const PrimitiveConversionState& state,
                                        std::span<const std::uint8_t> premultiplied) {
  image.pixels.assign(premultiplied.begin(), premultiplied.end());
  image.width = primitive.imageWidth;
  image.height = primitive.imageHeight;
  const double deviceOffsetX =
      primitive.fragmentRegionTopLeft.x * state.context.filterGraph.userToPixelScale.x;
  const double deviceOffsetY =
      primitive.fragmentRegionTopLeft.y * state.context.filterGraph.userToPixelScale.y;
  image.targetRect = tiny_skia::filter::PixelRect{deviceOffsetX, deviceOffsetY,
                                                  static_cast<double>(primitive.imageWidth),
                                                  static_cast<double>(primitive.imageHeight)};
}

void PopulateTransformedImage(gp::Image& image, const fp::Image& primitive,
                              const PrimitiveConversionState& state,
                              std::span<const std::uint8_t> premultiplied) {
  const Box2d imageBox = Box2d::FromXYWH(0, 0, primitive.imageWidth, primitive.imageHeight);
  const Box2d userSubregion =
      state.node.userSpaceSubregion.has_value()
          ? Box2d::FromXYWH(state.node.userSpaceSubregion->x, state.node.userSpaceSubregion->y,
                            state.node.userSpaceSubregion->w, state.node.userSpaceSubregion->h)
          : state.context.filterRegion.value_or(state.context.primitiveUnitsBounds);
  const Transform2d filterFromImage =
      primitive.preserveAspectRatio.elementContentFromViewBoxTransform(userSubregion, imageBox);
  const Transform2d deviceFromImage = filterFromImage * state.context.deviceFromFilter;

  image.pixels = RasterizeImagePremultiplied(
      premultiplied, primitive.imageWidth, primitive.imageHeight, deviceFromImage,
      state.context.outputWidth, state.context.outputHeight, primitive.imageRendering);
  image.width = state.context.outputWidth;
  image.height = state.context.outputHeight;
  image.targetRect =
      tiny_skia::filter::PixelRect{0.0, 0.0, static_cast<double>(state.context.outputWidth),
                                   static_cast<double>(state.context.outputHeight)};
}

tiny_skia::filter::PixelRect ResolveUntransformedImageTarget(
    const fp::Image& primitive, const PrimitiveConversionState& state) {
  double regionX = 0.0;
  double regionY = 0.0;
  double regionWidth = state.context.outputWidth;
  double regionHeight = state.context.outputHeight;
  if (state.node.subregion.has_value()) {
    regionX = state.node.subregion->x;
    regionY = state.node.subregion->y;
    regionWidth = state.node.subregion->w;
    regionHeight = state.node.subregion->h;
  } else if (state.graph.filterRegion.has_value()) {
    regionX = state.graph.filterRegion->x;
    regionY = state.graph.filterRegion->y;
    regionWidth = state.graph.filterRegion->w;
    regionHeight = state.graph.filterRegion->h;
  }

  const Box2d imageBox = Box2d::FromXYWH(0, 0, primitive.imageWidth, primitive.imageHeight);
  const Box2d regionRect = Box2d::FromXYWH(0, 0, regionWidth, regionHeight);
  const Transform2d regionFromImage =
      primitive.preserveAspectRatio.elementContentFromViewBoxTransform(regionRect, imageBox);
  const Vector2d topLeft = regionFromImage.transformPosition(Vector2d(0, 0));
  const Vector2d bottomRight =
      regionFromImage.transformPosition(Vector2d(primitive.imageWidth, primitive.imageHeight));
  return tiny_skia::filter::PixelRect{
      std::min(topLeft.x, bottomRight.x) + regionX, std::min(topLeft.y, bottomRight.y) + regionY,
      std::abs(bottomRight.x - topLeft.x), std::abs(bottomRight.y - topLeft.y)};
}

void PopulateUntransformedImage(gp::Image& image, const fp::Image& primitive,
                                const PrimitiveConversionState& state,
                                std::span<const std::uint8_t> premultiplied) {
  image.pixels.assign(premultiplied.begin(), premultiplied.end());
  image.width = primitive.imageWidth;
  image.height = primitive.imageHeight;
  image.targetRect = ResolveUntransformedImageTarget(primitive, state);
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::Image& primitive,
                                                   const PrimitiveConversionState& state) {
  gp::Image image;
  image.sampling = ConvertImageSampling(primitive.imageRendering);
  const std::span<const uint8_t> imageData = primitive.imageData
                                                 ? std::span<const uint8_t>(*primitive.imageData)
                                                 : std::span<const uint8_t>();
  if (!HasExactRgbaPayload(imageData, primitive.imageWidth, primitive.imageHeight)) {
    return image;
  }

  const std::vector<std::uint8_t> premultiplied = PremultiplyRgba(imageData);
  if (primitive.isFragmentReference && state.graph.filterFromDevice.has_value()) {
    PopulateTransformedFragmentImage(image, primitive, state, premultiplied);
  } else if (primitive.isFragmentReference) {
    PopulateUntransformedFragmentImage(image, primitive, state, premultiplied);
  } else if (state.graph.filterFromDevice.has_value()) {
    PopulateTransformedImage(image, primitive, state, premultiplied);
  } else {
    PopulateUntransformedImage(image, primitive, state, premultiplied);
  }
  return image;
}

template <typename LightingParams>
void PopulateLightingParams(LightingParams& params, const css::Color& lightingColor,
                            const fp::LightSource& light, const FilterConversionContext& context) {
  const css::RGBA rgba = lightingColor.asRGBA();
  params.lightR = rgba.r / 255.0;
  params.lightG = rgba.g / 255.0;
  params.lightB = rgba.b / 255.0;
  params.light = context.convertLightSource(light);
  params.pixelToUser = context.pixelToUser;
  params.hasShear = context.hasShear;
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::DiffuseLighting& primitive,
                                                   const PrimitiveConversionState& state) {
  if (!primitive.light.has_value()) {
    return gp::Image{};
  }
  gp::DiffuseLighting diffuseLighting;
  diffuseLighting.params.surfaceScale = primitive.surfaceScale;
  diffuseLighting.params.diffuseConstant = primitive.diffuseConstant;
  PopulateLightingParams(diffuseLighting.params, primitive.lightingColor, *primitive.light,
                         state.context);
  return diffuseLighting;
}

tiny_skia::filter::GraphPrimitive ConvertPrimitive(const fp::SpecularLighting& primitive,
                                                   const PrimitiveConversionState& state) {
  if (!primitive.light.has_value()) {
    return gp::Image{};
  }
  gp::SpecularLighting specularLighting;
  specularLighting.params.surfaceScale = primitive.surfaceScale;
  specularLighting.params.specularConstant = primitive.specularConstant;
  specularLighting.params.specularExponent = primitive.specularExponent;
  PopulateLightingParams(specularLighting.params, primitive.lightingColor, *primitive.light,
                         state.context);
  return specularLighting;
}

tiny_skia::filter::FilterGraph CreateExecutableGraph(const FilterConversionContext& context,
                                                     bool clipSourceToFilterRegion,
                                                     const tiny_skia::Pixmap* fillPaintInput,
                                                     const tiny_skia::Pixmap* strokePaintInput) {
  tiny_skia::filter::FilterGraph graph;
  graph.useLinearRGB =
      context.filterGraph.colorInterpolationFilters != ColorInterpolationFilters::SRGB;
  graph.clipSourceToFilterRegion = clipSourceToFilterRegion;
  if (fillPaintInput != nullptr) {
    graph.fillPaintInput = *fillPaintInput;
  }
  if (strokePaintInput != nullptr) {
    graph.strokePaintInput = *strokePaintInput;
  }

  if (context.filterRegion.has_value()) {
    const Box2d pixelRegion = context.deviceFromFilter.transformBox(*context.filterRegion);
    graph.filterRegion = BoundedPixelRect(pixelRegion, context.outputWidth, context.outputHeight);
  }

  const bool hasRotation = !NearZero(context.deviceFromFilter.data[1], 1e-6) ||
                           !NearZero(context.deviceFromFilter.data[2], 1e-6);
  if (hasRotation && !NearZero(context.deviceFromFilter.determinant(), 1e-12)) {
    const Transform2d filterFromDevice = context.deviceFromFilter.inverse();
    graph.filterFromDevice = tiny_skia::filter::AffineTransform{
        filterFromDevice.data[0], filterFromDevice.data[1], filterFromDevice.data[2],
        filterFromDevice.data[3], filterFromDevice.data[4], filterFromDevice.data[5]};
    if (context.filterRegion.has_value()) {
      graph.userSpaceFilterRegion = tiny_skia::filter::PixelRect{
          context.filterRegion->topLeft.x, context.filterRegion->topLeft.y,
          context.filterRegion->width(), context.filterRegion->height()};
    }
  }
  return graph;
}

void PopulateSubregion(const components::FilterNode& node, const FilterConversionContext& context,
                       const tiny_skia::filter::FilterGraph& graph,
                       tiny_skia::filter::GraphNode& graphNode) {
  if (!node.x.has_value() && !node.y.has_value() && !node.width.has_value() &&
      !node.height.has_value()) {
    return;
  }

  const Box2d defaultSubregionUser = context.filterRegion.value_or(context.primitiveUnitsBounds);
  const double userX = node.x.has_value()
                           ? context.resolvePrimitivePosition(*node.x, Lengthd::Extent::X)
                           : defaultSubregionUser.topLeft.x;
  const double userY = node.y.has_value()
                           ? context.resolvePrimitivePosition(*node.y, Lengthd::Extent::Y)
                           : defaultSubregionUser.topLeft.y;
  const double userWidth = node.width.has_value()
                               ? context.resolvePrimitiveSize(*node.width, Lengthd::Extent::X)
                               : defaultSubregionUser.width();
  const double userHeight = node.height.has_value()
                                ? context.resolvePrimitiveSize(*node.height, Lengthd::Extent::Y)
                                : defaultSubregionUser.height();

  const Box2d pixelRegion =
      context.deviceFromFilter.transformBox(Box2d::FromXYWH(userX, userY, userWidth, userHeight));
  graphNode.subregion = BoundedPixelRect(pixelRegion, context.outputWidth, context.outputHeight);
  if (graph.filterFromDevice.has_value()) {
    graphNode.userSpaceSubregion =
        tiny_skia::filter::PixelRect{userX, userY, userWidth, userHeight};
  }
}

tiny_skia::filter::GraphNode ConvertNode(const components::FilterNode& node,
                                         const FilterConversionContext& context,
                                         const tiny_skia::filter::FilterGraph& graph) {
  tiny_skia::filter::GraphNode graphNode;
  graphNode.inputs.reserve(node.inputs.size());
  for (const components::FilterInput& input : node.inputs) {
    graphNode.inputs.push_back(ConvertInput(input));
  }
  if (node.result.has_value()) {
    graphNode.result = std::string(*node.result);
  }

  PopulateSubregion(node, context, graph, graphNode);
  const PrimitiveConversionState state{context, graph, graphNode};
  graphNode.primitive =
      std::visit([&state](const auto& primitive) { return ConvertPrimitive(primitive, state); },
                 node.primitive);

  if (node.colorInterpolationFilters.has_value()) {
    graphNode.useLinearRGB = *node.colorInterpolationFilters != ColorInterpolationFilters::SRGB;
  }
  return graphNode;
}

}  // namespace

void ApplyFilterGraphToPixmap(tiny_skia::Pixmap& pixmap, const components::FilterGraph& filterGraph,
                              const Transform2d& deviceFromFilter,
                              const std::optional<Box2d>& filterRegion,
                              bool clipSourceToFilterRegion,
                              const tiny_skia::Pixmap* fillPaintInput,
                              const tiny_skia::Pixmap* strokePaintInput,
                              components::FilterExecutionBudget* executionBudget) {
  const std::uint64_t width = pixmap.width();
  const std::uint64_t height = pixmap.height();
  const std::uint64_t pixelCount = width * height;
  const bool fitsBudget =
      executionBudget != nullptr
          ? executionBudget->consume(filterGraph, pixelCount,
                                     components::FilterMemoryModel::CpuFloatNamedResults)
          : components::FilterGraphFitsExecutionBudget(
                filterGraph, pixelCount, components::FilterMemoryModel::CpuFloatNamedResults);
  if (!fitsBudget) {
    return;
  }
  const FilterConversionContext context(filterGraph, deviceFromFilter, filterRegion,
                                        static_cast<int>(pixmap.width()),
                                        static_cast<int>(pixmap.height()));
  tiny_skia::filter::FilterGraph graph =
      CreateExecutableGraph(context, clipSourceToFilterRegion, fillPaintInput, strokePaintInput);
  graph.nodes.reserve(filterGraph.nodes.size());
  for (const components::FilterNode& node : filterGraph.nodes) {
    graph.nodes.push_back(ConvertNode(node, context, graph));
  }

  tiny_skia::filter::executeFilterGraph(pixmap, graph);
}

void ClipFilterOutputToRegion(tiny_skia::Pixmap& pixmap, const std::optional<Box2d>& filterRegion,
                              const Transform2d& deviceFromFilter) {
  if (!filterRegion.has_value()) {
    return;
  }

  const Vector2d transformedXAxis = deviceFromFilter.transformVector(Vector2d(1.0, 0.0));
  const Vector2d transformedYAxis = deviceFromFilter.transformVector(Vector2d(0.0, 1.0));
  const double dot =
      transformedXAxis.x * transformedYAxis.x + transformedXAxis.y * transformedYAxis.y;
  const bool hasNonAxisAlignedTransform = !NearZero(dot, 1e-6) ||
                                          !NearZero(deviceFromFilter.data[1], 1e-6) ||
                                          !NearZero(deviceFromFilter.data[2], 1e-6);

  if (hasNonAxisAlignedTransform && !NearZero(deviceFromFilter.determinant(), 1e-12)) {
    const Transform2d filterFromDevice = deviceFromFilter.inverse();
    const int width = static_cast<int>(pixmap.width());
    const int height = static_cast<int>(pixmap.height());
    auto data = pixmap.data();
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const Vector2d filterPoint = filterFromDevice.transformPosition(Vector2d(x + 0.5, y + 0.5));
        if (filterPoint.x < filterRegion->topLeft.x ||
            filterPoint.x >= filterRegion->bottomRight.x ||
            filterPoint.y < filterRegion->topLeft.y ||
            filterPoint.y >= filterRegion->bottomRight.y) {
          const std::size_t idx = static_cast<std::size_t>((y * width + x) * 4);
          data[idx + 0] = 0;
          data[idx + 1] = 0;
          data[idx + 2] = 0;
          data[idx + 3] = 0;
        }
      }
    }
    return;
  }

  const Box2d pixelRegion = deviceFromFilter.transformBox(*filterRegion);
  const int width = static_cast<int>(pixmap.width());
  const int height = static_cast<int>(pixmap.height());
  // A pixel belongs to the filter region when its center lies inside the half-open rectangle.
  // This is the same rule used by the non-axis-aligned path above. Convert each continuous edge to
  // the first integer pixel index whose center is on or beyond that edge. Clamp both sides so a
  // fully offscreen region collapses to an empty kept rectangle without an out-of-bounds clear.
  const auto firstPixelAtOrBeyond = [](double edge, int limit) {
    if (std::isnan(edge) || edge <= 0.5) {
      return 0;
    }
    if (!std::isfinite(edge) || edge >= static_cast<double>(limit) + 0.5) {
      return limit;
    }
    return static_cast<int>(std::ceil(edge - 0.5));
  };
  const int x0 = firstPixelAtOrBeyond(pixelRegion.topLeft.x, width);
  const int y0 = firstPixelAtOrBeyond(pixelRegion.topLeft.y, height);
  const int x1 = firstPixelAtOrBeyond(pixelRegion.bottomRight.x, width);
  const int y1 = firstPixelAtOrBeyond(pixelRegion.bottomRight.y, height);

  auto data = pixmap.data();
  for (int y = 0; y < y0; ++y) {
    std::fill_n(data.data() + y * width * 4, width * 4, std::uint8_t{0});
  }
  for (int y = y0; y < y1; ++y) {
    if (x0 > 0) {
      std::fill_n(data.data() + y * width * 4, x0 * 4, std::uint8_t{0});
    }
    if (x1 < width) {
      std::fill_n(data.data() + (y * width + x1) * 4, (width - x1) * 4, std::uint8_t{0});
    }
  }
  for (int y = y1; y < height; ++y) {
    std::fill_n(data.data() + y * width * 4, width * 4, std::uint8_t{0});
  }
}

}  // namespace donner::svg
