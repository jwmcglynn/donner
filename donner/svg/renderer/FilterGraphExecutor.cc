#include "donner/svg/renderer/FilterGraphExecutor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "tiny_skia/filter/FilterGraph.h"

namespace donner::svg {

std::vector<std::uint8_t> PremultiplyRgba(std::span<const std::uint8_t> rgbaPixels) {
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

void ApplyFilterGraphToPixmap(tiny_skia::Pixmap& pixmap, const components::FilterGraph& filterGraph,
                              const Transformd& deviceFromFilter,
                              const std::optional<Boxd>& filterRegion,
                              bool clipSourceToFilterRegion,
                              const tiny_skia::Pixmap* fillPaintInput,
                              const tiny_skia::Pixmap* strokePaintInput) {
  using namespace components;
  using namespace components::filter_primitive;
  namespace gp = tiny_skia::filter::graph_primitive;

  const int w = static_cast<int>(pixmap.width());
  const int h = static_cast<int>(pixmap.height());

  const bool isOBB = filterGraph.primitiveUnits == PrimitiveUnits::ObjectBoundingBox &&
                     filterGraph.elementBoundingBox.has_value();
  const double bboxW = isOBB ? filterGraph.elementBoundingBox->width() : 1.0;
  const double bboxH = isOBB ? filterGraph.elementBoundingBox->height() : 1.0;
  const double bboxX = isOBB ? filterGraph.elementBoundingBox->topLeft.x : 0.0;
  const double bboxY = isOBB ? filterGraph.elementBoundingBox->topLeft.y : 0.0;

  const Vector2d transformedXAxis = deviceFromFilter.transformVector(Vector2d(1.0, 0.0));
  const double scaleX = transformedXAxis.length();
  const double determinant = deviceFromFilter.determinant();
  const double scaleY =
      NearZero(scaleX, 1e-12) ? std::abs(deviceFromFilter.data[3]) : std::abs(determinant) / scaleX;

  auto toPixelX = [&](double userX) -> double { return std::abs(userX) * scaleX; };
  auto toPixelY = [&](double userY) -> double { return std::abs(userY) * scaleY; };

  auto primToPixelX = [&](double val) -> double {
    return isOBB ? toPixelX(val * bboxW) : toPixelX(val);
  };
  auto primToPixelY = [&](double val) -> double {
    return isOBB ? toPixelY(val * bboxH) : toPixelY(val);
  };
  auto primToPixelOffset = [&](double dx, double dy) -> Vector2d {
    const Vector2d userOffset = isOBB ? Vector2d(dx * bboxW, dy * bboxH) : Vector2d(dx, dy);
    return deviceFromFilter.transformVector(userOffset);
  };

  const Boxd primitiveUnitsBounds = [&]() {
    if (isOBB) {
      return *filterGraph.elementBoundingBox;
    }
    const double userW =
        NearZero(filterGraph.userToPixelScale.x, 1e-12)
            ? (NearZero(scaleX, 1e-12) ? static_cast<double>(w) : static_cast<double>(w) / scaleX)
            : static_cast<double>(w) / filterGraph.userToPixelScale.x;
    const double userH =
        NearZero(filterGraph.userToPixelScale.y, 1e-12)
            ? (NearZero(scaleY, 1e-12) ? static_cast<double>(h) : static_cast<double>(h) / scaleY)
            : static_cast<double>(h) / filterGraph.userToPixelScale.y;
    return Boxd::FromXYWH(0.0, 0.0, userW, userH);
  }();

  auto percentPositionReferenceSize = [&](Lengthd::Extent extent) -> double {
    if (isOBB) {
      return extent == Lengthd::Extent::X ? bboxW : bboxH;
    }
    return extent == Lengthd::Extent::X ? primitiveUnitsBounds.width()
                                        : primitiveUnitsBounds.height();
  };

  auto percentPositionReferenceOrigin = [&](Lengthd::Extent extent) -> double {
    if (isOBB) {
      return extent == Lengthd::Extent::X ? bboxX : bboxY;
    }
    return extent == Lengthd::Extent::X ? primitiveUnitsBounds.topLeft.x
                                        : primitiveUnitsBounds.topLeft.y;
  };

  auto percentSizeReference = [&](Lengthd::Extent extent) -> double {
    if (isOBB) {
      return extent == Lengthd::Extent::X ? bboxW : bboxH;
    }
    return extent == Lengthd::Extent::X ? primitiveUnitsBounds.width()
                                        : primitiveUnitsBounds.height();
  };

  auto resolvePrimitivePosition = [&](const Lengthd& len, Lengthd::Extent extent, double origin,
                                      double bboxDim) -> double {
    if (isOBB && len.unit == Lengthd::Unit::None) {
      return origin + len.value * bboxDim;
    }
    if (len.unit == Lengthd::Unit::Percent) {
      return percentPositionReferenceOrigin(extent) +
             percentPositionReferenceSize(extent) * len.value / 100.0;
    }
    return len.toPixels(primitiveUnitsBounds, FontMetrics(), extent);
  };

  auto resolvePrimitiveSize = [&](const Lengthd& len, Lengthd::Extent extent,
                                  double bboxDim) -> double {
    if (isOBB && len.unit == Lengthd::Unit::None) {
      return len.value * bboxDim;
    }
    if (len.unit == Lengthd::Unit::Percent) {
      return percentSizeReference(extent) * len.value / 100.0;
    }
    return len.toPixels(primitiveUnitsBounds, FontMetrics(), extent);
  };

  auto convertInput = [](const FilterInput& fi) -> tiny_skia::filter::NodeInput {
    return std::visit(
        [](const auto& v) -> tiny_skia::filter::NodeInput {
          using V = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<V, FilterInput::Previous>) {
            return tiny_skia::filter::NodeInput();
          } else if constexpr (std::is_same_v<V, FilterStandardInput>) {
            return tiny_skia::filter::NodeInput(static_cast<tiny_skia::filter::StandardInput>(v));
          } else if constexpr (std::is_same_v<V, FilterInput::Named>) {
            return tiny_skia::filter::NodeInput(
                tiny_skia::filter::NodeInput::Named{std::string(v.name)});
          } else {
            return tiny_skia::filter::NodeInput();
          }
        },
        fi.value);
  };

  struct PremulRGBA {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
  };
  auto floodToPremul = [](const css::Color& color, double opacity) -> PremulRGBA {
    const css::RGBA rgba = color.asRGBA();
    const double alpha = (rgba.a / 255.0) * opacity;
    return {static_cast<std::uint8_t>(std::round(rgba.r * alpha)),
            static_cast<std::uint8_t>(std::round(rgba.g * alpha)),
            static_cast<std::uint8_t>(std::round(rgba.b * alpha)),
            static_cast<std::uint8_t>(std::round(alpha * 255.0))};
  };

  const double pixelScale = [&]() -> double {
    const Vector2d sx = deviceFromFilter.transformPosition(Vector2d(1, 0)) -
                        deviceFromFilter.transformPosition(Vector2d(0, 0));
    const Vector2d sy = deviceFromFilter.transformPosition(Vector2d(0, 1)) -
                        deviceFromFilter.transformPosition(Vector2d(0, 0));
    return std::sqrt((sx.x * sx.x + sx.y * sx.y + sy.x * sy.x + sy.y * sy.y) / 2.0);
  }();

  auto convertLightSource =
      [&](const filter_primitive::LightSource& ls) -> tiny_skia::filter::LightSourceParams {
    tiny_skia::filter::LightSourceParams lp;
    lp.type = static_cast<tiny_skia::filter::LightType>(ls.type);
    lp.azimuth = ls.azimuth;
    lp.elevation = ls.elevation;

    double userX;
    double userY;
    double userZ;
    double userPtX;
    double userPtY;
    double userPtZ;
    if (isOBB) {
      userX = ls.x * bboxW + bboxX;
      userY = ls.y * bboxH + bboxY;
      userZ = ls.z * bboxH;
      userPtX = ls.pointsAtX * bboxW + bboxX;
      userPtY = ls.pointsAtY * bboxH + bboxY;
      userPtZ = ls.pointsAtZ * bboxH;
    } else {
      userX = ls.x;
      userY = ls.y;
      userZ = ls.z;
      userPtX = ls.pointsAtX;
      userPtY = ls.pointsAtY;
      userPtZ = ls.pointsAtZ;
    }

    const Vector2d lightPixel = deviceFromFilter.transformPosition(Vector2d(userX, userY));
    const Vector2d pointsAtPixel = deviceFromFilter.transformPosition(Vector2d(userPtX, userPtY));
    lp.x = lightPixel.x;
    lp.y = lightPixel.y;
    lp.z = userZ * pixelScale;
    lp.pointsAtX = pointsAtPixel.x;
    lp.pointsAtY = pointsAtPixel.y;
    lp.pointsAtZ = userPtZ * pixelScale;
    lp.spotExponent = ls.spotExponent;
    lp.limitingConeAngle = ls.limitingConeAngle;
    return lp;
  };

  tiny_skia::filter::FilterGraph graph;
  graph.useLinearRGB = filterGraph.colorInterpolationFilters != ColorInterpolationFilters::SRGB;
  graph.clipSourceToFilterRegion = clipSourceToFilterRegion;
  if (fillPaintInput != nullptr) {
    graph.fillPaintInput = tiny_skia::filter::FloatPixmap::fromPixmap(*fillPaintInput);
  }
  if (strokePaintInput != nullptr) {
    graph.strokePaintInput = tiny_skia::filter::FloatPixmap::fromPixmap(*strokePaintInput);
  }

  if (filterRegion.has_value()) {
    const Boxd pixelRegion = deviceFromFilter.transformBox(*filterRegion);
    graph.filterRegion =
        tiny_skia::filter::PixelRect{pixelRegion.topLeft.x, pixelRegion.topLeft.y,
                                     pixelRegion.bottomRight.x - pixelRegion.topLeft.x,
                                     pixelRegion.bottomRight.y - pixelRegion.topLeft.y};
  }

  for (const FilterNode& node : filterGraph.nodes) {
    tiny_skia::filter::GraphNode graphNode;

    for (const auto& fi : node.inputs) {
      graphNode.inputs.push_back(convertInput(fi));
    }

    if (node.result.has_value()) {
      graphNode.result = std::string(*node.result);
    }

    if (node.x.has_value() || node.y.has_value() || node.width.has_value() ||
        node.height.has_value()) {
      const Boxd defaultSubregionUser = filterRegion.value_or(primitiveUnitsBounds);
      const double defaultUserWidth = defaultSubregionUser.width();
      const double defaultUserHeight = defaultSubregionUser.height();
      const double defaultOriginX = defaultSubregionUser.topLeft.x;
      const double defaultOriginY = defaultSubregionUser.topLeft.y;

      const double ux = node.x.has_value() ? resolvePrimitivePosition(*node.x, Lengthd::Extent::X,
                                                                      isOBB ? bboxX : 0.0, bboxW)
                                           : defaultOriginX;
      const double uy = node.y.has_value() ? resolvePrimitivePosition(*node.y, Lengthd::Extent::Y,
                                                                      isOBB ? bboxY : 0.0, bboxH)
                                           : defaultOriginY;
      const double uw = node.width.has_value()
                            ? resolvePrimitiveSize(*node.width, Lengthd::Extent::X, bboxW)
                            : defaultUserWidth;
      const double uh = node.height.has_value()
                            ? resolvePrimitiveSize(*node.height, Lengthd::Extent::Y, bboxH)
                            : defaultUserHeight;

      const Boxd pixelRegion = deviceFromFilter.transformBox(Boxd::FromXYWH(ux, uy, uw, uh));
      graphNode.subregion =
          tiny_skia::filter::PixelRect{pixelRegion.topLeft.x, pixelRegion.topLeft.y,
                                       pixelRegion.bottomRight.x - pixelRegion.topLeft.x,
                                       pixelRegion.bottomRight.y - pixelRegion.topLeft.y};
    }

    std::visit(
        [&](const auto& primitive) {
          using T = std::decay_t<decltype(primitive)>;

          if constexpr (std::is_same_v<T, GaussianBlur>) {
            gp::GaussianBlur blur;
            blur.sigmaX =
                primitive.stdDeviationX >= 0 ? primToPixelX(primitive.stdDeviationX) : 0.0;
            blur.sigmaY =
                primitive.stdDeviationY >= 0 ? primToPixelY(primitive.stdDeviationY) : 0.0;
            blur.edgeMode = static_cast<tiny_skia::filter::BlurEdgeMode>(primitive.edgeMode);
            graphNode.primitive = blur;

          } else if constexpr (std::is_same_v<T, Flood>) {
            const PremulRGBA pm = floodToPremul(primitive.floodColor, primitive.floodOpacity);
            graphNode.primitive = gp::Flood{pm.r, pm.g, pm.b, pm.a};

          } else if constexpr (std::is_same_v<T, Offset>) {
            const Vector2d offset = primToPixelOffset(primitive.dx, primitive.dy);
            graphNode.primitive = gp::Offset{
                static_cast<int>(std::lround(offset.x)),
                static_cast<int>(std::lround(offset.y)),
            };

          } else if constexpr (std::is_same_v<T, Composite>) {
            gp::Composite composite;
            composite.op = static_cast<tiny_skia::filter::CompositeOp>(primitive.op);
            composite.k1 = primitive.k1;
            composite.k2 = primitive.k2;
            composite.k3 = primitive.k3;
            composite.k4 = primitive.k4;
            graphNode.primitive = composite;

          } else if constexpr (std::is_same_v<T, Blend>) {
            gp::Blend blend;
            blend.mode = static_cast<tiny_skia::filter::BlendMode>(primitive.mode);
            graphNode.primitive = blend;

          } else if constexpr (std::is_same_v<T, Merge>) {
            graphNode.primitive = gp::Merge{};

          } else if constexpr (std::is_same_v<T, ColorMatrix>) {
            gp::ColorMatrix colorMatrix;
            if (primitive.type == ColorMatrix::Type::Matrix) {
              if (primitive.values.size() == 20) {
                for (size_t j = 0; j < 20; ++j) {
                  colorMatrix.matrix[j] = primitive.values[j];
                }
              } else {
                colorMatrix.matrix = tiny_skia::filter::identityMatrix();
              }
            } else if (primitive.type == ColorMatrix::Type::Saturate) {
              const double s = primitive.values.empty() ? 1.0 : primitive.values[0];
              colorMatrix.matrix = tiny_skia::filter::saturateMatrix(s);
            } else if (primitive.type == ColorMatrix::Type::HueRotate) {
              const double angle = primitive.values.empty() ? 0.0 : primitive.values[0];
              colorMatrix.matrix = tiny_skia::filter::hueRotateMatrix(angle);
            } else if (primitive.type == ColorMatrix::Type::LuminanceToAlpha) {
              colorMatrix.matrix = tiny_skia::filter::luminanceToAlphaMatrix();
            } else {
              colorMatrix.matrix = tiny_skia::filter::identityMatrix();
            }
            graphNode.primitive = colorMatrix;

          } else if constexpr (std::is_same_v<T, ComponentTransfer>) {
            gp::ComponentTransfer componentTransfer;
            auto convertFunc = [](const ComponentTransfer::Func& f) {
              gp::ComponentTransfer::Func graphFunc;
              graphFunc.type = static_cast<tiny_skia::filter::TransferFuncType>(f.type);
              graphFunc.tableValues = f.tableValues;
              graphFunc.slope = f.slope;
              graphFunc.intercept = f.intercept;
              graphFunc.amplitude = f.amplitude;
              graphFunc.exponent = f.exponent;
              graphFunc.offset = f.offset;
              return graphFunc;
            };
            componentTransfer.funcR = convertFunc(primitive.funcR);
            componentTransfer.funcG = convertFunc(primitive.funcG);
            componentTransfer.funcB = convertFunc(primitive.funcB);
            componentTransfer.funcA = convertFunc(primitive.funcA);
            graphNode.primitive = componentTransfer;

          } else if constexpr (std::is_same_v<T, DropShadow>) {
            const PremulRGBA pm = floodToPremul(primitive.floodColor, primitive.floodOpacity);
            gp::DropShadow dropShadow;
            dropShadow.r = pm.r;
            dropShadow.g = pm.g;
            dropShadow.b = pm.b;
            dropShadow.a = pm.a;
            const Vector2d offset = primToPixelOffset(primitive.dx, primitive.dy);
            dropShadow.dx = static_cast<int>(std::lround(offset.x));
            dropShadow.dy = static_cast<int>(std::lround(offset.y));
            dropShadow.sigmaX =
                primitive.stdDeviationX >= 0 ? primToPixelX(primitive.stdDeviationX) : 0.0;
            dropShadow.sigmaY =
                primitive.stdDeviationY >= 0 ? primToPixelY(primitive.stdDeviationY) : 0.0;
            graphNode.primitive = dropShadow;

          } else if constexpr (std::is_same_v<T, Morphology>) {
            gp::Morphology morphology;
            if (primitive.radiusX < 0 || primitive.radiusY < 0 ||
                (primitive.radiusX == 0 && primitive.radiusY == 0)) {
              morphology.radiusX = 0;
              morphology.radiusY = 0;
            } else {
              morphology.op = primitive.op == Morphology::Operator::Erode
                                  ? tiny_skia::filter::MorphologyOp::Erode
                                  : tiny_skia::filter::MorphologyOp::Dilate;
              morphology.radiusX = static_cast<int>(std::round(primToPixelX(primitive.radiusX)));
              morphology.radiusY = static_cast<int>(std::round(primToPixelY(primitive.radiusY)));
            }
            graphNode.primitive = morphology;

          } else if constexpr (std::is_same_v<T, ConvolveMatrix>) {
            gp::ConvolveMatrix convolveMatrix;
            convolveMatrix.orderX = primitive.orderX;
            convolveMatrix.orderY = primitive.orderY;
            convolveMatrix.kernel = primitive.kernelMatrix;
            convolveMatrix.bias = primitive.bias;
            convolveMatrix.edgeMode =
                static_cast<tiny_skia::filter::ConvolveEdgeMode>(primitive.edgeMode);
            convolveMatrix.preserveAlpha = primitive.preserveAlpha;
            convolveMatrix.targetX = primitive.targetX.value_or(primitive.orderX / 2);
            convolveMatrix.targetY = primitive.targetY.value_or(primitive.orderY / 2);

            if (primitive.divisor.has_value()) {
              convolveMatrix.divisor = *primitive.divisor;
            } else {
              const int requiredSize = primitive.orderX * primitive.orderY;
              double sum = 0.0;
              for (size_t i = 0;
                   i < primitive.kernelMatrix.size() && i < static_cast<size_t>(requiredSize);
                   ++i) {
                sum += primitive.kernelMatrix[i];
              }
              convolveMatrix.divisor = sum == 0.0 ? 1.0 : sum;
            }
            graphNode.primitive = convolveMatrix;

          } else if constexpr (std::is_same_v<T, Tile>) {
            graphNode.primitive = gp::Tile{};

          } else if constexpr (std::is_same_v<T, Turbulence>) {
            gp::Turbulence turbulence;
            turbulence.params.type = primitive.type == Turbulence::Type::FractalNoise
                                         ? tiny_skia::filter::TurbulenceType::FractalNoise
                                         : tiny_skia::filter::TurbulenceType::Turbulence;
            turbulence.params.baseFrequencyX = primitive.baseFrequencyX;
            turbulence.params.baseFrequencyY = primitive.baseFrequencyY;
            turbulence.params.numOctaves = primitive.numOctaves;
            turbulence.params.seed = primitive.seed;
            turbulence.params.stitchTiles = primitive.stitchTiles;
            turbulence.params.tileWidth = w;
            turbulence.params.tileHeight = h;
            turbulence.params.scaleX = std::abs(deviceFromFilter.data[0]);
            turbulence.params.scaleY = std::abs(deviceFromFilter.data[3]);
            if (turbulence.params.scaleX < 1e-10) {
              turbulence.params.scaleX = 1.0;
            }
            if (turbulence.params.scaleY < 1e-10) {
              turbulence.params.scaleY = 1.0;
            }
            graphNode.primitive = turbulence;

          } else if constexpr (std::is_same_v<T, DisplacementMap>) {
            gp::DisplacementMap displacementMap;
            displacementMap.scale =
                isOBB ? primitive.scale * std::sqrt(bboxW * bboxH) : primitive.scale;
            displacementMap.xChannel =
                static_cast<tiny_skia::filter::DisplacementChannel>(primitive.xChannelSelector);
            displacementMap.yChannel =
                static_cast<tiny_skia::filter::DisplacementChannel>(primitive.yChannelSelector);
            graphNode.primitive = displacementMap;

          } else if constexpr (std::is_same_v<T, Image>) {
            gp::Image image;
            if (!primitive.imageData.empty() && primitive.imageWidth > 0 &&
                primitive.imageHeight > 0) {
              image.pixels = PremultiplyRgba(primitive.imageData);
              image.width = primitive.imageWidth;
              image.height = primitive.imageHeight;

              if (primitive.isFragmentReference) {
                // Fragment references are rendered in the same coordinate space as the filter
                // pixmap. Place the image at (0,0) with 1:1 pixel mapping — no
                // preserveAspectRatio scaling needed.
                image.targetRect = tiny_skia::filter::PixelRect{0.0, 0.0,
                    static_cast<double>(primitive.imageWidth),
                    static_cast<double>(primitive.imageHeight)};
              } else {
                double regionX = 0.0;
                double regionY = 0.0;
                double regionW = w;
                double regionH = h;
                if (graphNode.subregion.has_value()) {
                  regionX = graphNode.subregion->x;
                  regionY = graphNode.subregion->y;
                  regionW = graphNode.subregion->w;
                  regionH = graphNode.subregion->h;
                } else if (graph.filterRegion.has_value()) {
                  regionX = graph.filterRegion->x;
                  regionY = graph.filterRegion->y;
                  regionW = graph.filterRegion->w;
                  regionH = graph.filterRegion->h;
                }

                const Boxd imageBox =
                    Boxd::FromXYWH(0, 0, primitive.imageWidth, primitive.imageHeight);
                const Boxd regionRect = Boxd::FromXYWH(0, 0, regionW, regionH);
                const Transformd regionFromImage =
                    primitive.preserveAspectRatio.elementContentFromViewBoxTransform(regionRect,
                                                                                     imageBox);

                const Vector2d topLeft = regionFromImage.transformPosition(Vector2d(0, 0));
                const Vector2d bottomRight = regionFromImage.transformPosition(
                    Vector2d(primitive.imageWidth, primitive.imageHeight));
                image.targetRect = tiny_skia::filter::PixelRect{
                    std::min(topLeft.x, bottomRight.x) + regionX,
                    std::min(topLeft.y, bottomRight.y) + regionY,
                    std::abs(bottomRight.x - topLeft.x),
                    std::abs(bottomRight.y - topLeft.y)};
              }
            }
            graphNode.primitive = std::move(image);

          } else if constexpr (std::is_same_v<T, DiffuseLighting>) {
            if (primitive.light.has_value()) {
              gp::DiffuseLighting diffuseLighting;
              diffuseLighting.params.surfaceScale = primitive.surfaceScale;
              diffuseLighting.params.diffuseConstant = primitive.diffuseConstant;
              const css::RGBA rgba = primitive.lightingColor.asRGBA();
              diffuseLighting.params.lightR = rgba.r / 255.0;
              diffuseLighting.params.lightG = rgba.g / 255.0;
              diffuseLighting.params.lightB = rgba.b / 255.0;
              diffuseLighting.params.light = convertLightSource(*primitive.light);
              graphNode.primitive = diffuseLighting;
            } else {
              graphNode.primitive = gp::Image{};
            }

          } else if constexpr (std::is_same_v<T, SpecularLighting>) {
            if (primitive.light.has_value()) {
              gp::SpecularLighting specularLighting;
              specularLighting.params.surfaceScale = primitive.surfaceScale;
              specularLighting.params.specularConstant = primitive.specularConstant;
              specularLighting.params.specularExponent = primitive.specularExponent;
              const css::RGBA rgba = primitive.lightingColor.asRGBA();
              specularLighting.params.lightR = rgba.r / 255.0;
              specularLighting.params.lightG = rgba.g / 255.0;
              specularLighting.params.lightB = rgba.b / 255.0;
              specularLighting.params.light = convertLightSource(*primitive.light);
              graphNode.primitive = specularLighting;
            } else {
              graphNode.primitive = gp::Image{};
            }
          }
        },
        node.primitive);

    if (node.colorInterpolationFilters.has_value()) {
      graphNode.useLinearRGB = *node.colorInterpolationFilters != ColorInterpolationFilters::SRGB;
    }

    graph.nodes.push_back(std::move(graphNode));
  }

  tiny_skia::filter::executeFilterGraph(pixmap, graph);
}

void ClipFilterOutputToRegion(tiny_skia::Pixmap& pixmap, const std::optional<Boxd>& filterRegion,
                              const Transformd& deviceFromFilter) {
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
    const Transformd filterFromDevice = deviceFromFilter.inverse();
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

  const Boxd pixelRegion = deviceFromFilter.transformBox(*filterRegion);
  const int width = static_cast<int>(pixmap.width());
  const int height = static_cast<int>(pixmap.height());
  const int x0 = std::max(0, static_cast<int>(std::floor(pixelRegion.topLeft.x)));
  const int y0 = std::max(0, static_cast<int>(std::floor(pixelRegion.topLeft.y)));
  const int x1 = std::clamp(static_cast<int>(std::ceil(pixelRegion.bottomRight.x)), 0, width);
  const int y1 = std::clamp(static_cast<int>(std::ceil(pixelRegion.bottomRight.y)), 0, height);

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
