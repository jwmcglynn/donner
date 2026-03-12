#include "FilterGraph.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "tiny_skia/filter/Blend.h"
#include "tiny_skia/filter/ColorMatrix.h"
#include "tiny_skia/filter/ColorSpace.h"
#include "tiny_skia/filter/ComponentTransfer.h"
#include "tiny_skia/filter/Composite.h"
#include "tiny_skia/filter/ConvolveMatrix.h"
#include "tiny_skia/filter/DisplacementMap.h"
#include "tiny_skia/filter/Flood.h"
#include "tiny_skia/filter/FloatPixmap.h"
#include "tiny_skia/filter/GaussianBlur.h"
#include "tiny_skia/filter/Lighting.h"
#include "tiny_skia/filter/Merge.h"
#include "tiny_skia/filter/Morphology.h"
#include "tiny_skia/filter/Offset.h"
#include "tiny_skia/filter/Tile.h"
#include "tiny_skia/filter/Turbulence.h"

namespace tiny_skia::filter {

namespace {

/// Internal bounding box for subregion tracking (x0/y0 = top-left, x1/y1 = bottom-right).
struct Box {
  double x0 = 0;
  double y0 = 0;
  double x1 = 0;
  double y1 = 0;

  static Box fromPixelRect(const PixelRect& r) { return {r.x, r.y, r.x + r.w, r.y + r.h}; }

  static Box fromWH(int w, int h) {
    return {0.0, 0.0, static_cast<double>(w), static_cast<double>(h)};
  }

  [[nodiscard]] Box intersect(const Box& other) const {
    Box result;
    result.x0 = std::max(x0, other.x0);
    result.y0 = std::max(y0, other.y0);
    result.x1 = std::min(x1, other.x1);
    result.y1 = std::min(y1, other.y1);
    if (result.x1 < result.x0) {
      result.x1 = result.x0;
    }
    if (result.y1 < result.y0) {
      result.y1 = result.y0;
    }
    return result;
  }

  [[nodiscard]] Box unite(const Box& other) const {
    return {std::min(x0, other.x0), std::min(y0, other.y0), std::max(x1, other.x1),
            std::max(y1, other.y1)};
  }

  [[nodiscard]] Box translate(double dx, double dy) const {
    return {x0 + dx, y0 + dy, x1 + dx, y1 + dy};
  }

  [[nodiscard]] Box outset(double dx, double dy) const {
    return {x0 - dx, y0 - dy, x1 + dx, y1 + dy};
  }
};

Pixmap createTransparent(int w, int h) {
  auto pm = Pixmap::fromSize(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
  if (!pm.has_value()) {
    return Pixmap();
  }
  return std::move(*pm);
}

/// Copy a Pixmap (deep copy of pixel data).
Pixmap copyPixmap(const Pixmap& src) {
  auto dst = Pixmap::fromSize(src.width(), src.height());
  if (!dst.has_value()) {
    return Pixmap();
  }
  auto srcData = src.data();
  auto dstData = dst->data();
  std::copy(srcData.begin(), srcData.end(), dstData.begin());
  return std::move(*dst);
}

/// Convert uint8 sRGB Pixmap to float linear FloatPixmap (for linearRGB processing).
/// This avoids the "black crush" problem that occurs with uint8-to-uint8 LUT conversion.
FloatPixmap pixmapToLinearFloat(const Pixmap& pm) {
  auto fp = FloatPixmap::fromPixmap(pm);
  srgbToLinear(fp);
  return fp;
}

/// Convert float linear FloatPixmap to uint8 sRGB Pixmap (for inter-node storage).
Pixmap linearFloatToPixmap(FloatPixmap& fp) {
  linearToSrgb(fp);
  return fp.toPixmap();
}

std::optional<Box> computeNonTransparentBounds(const Pixmap& pixmap) {
  const int w = static_cast<int>(pixmap.width());
  const int h = static_cast<int>(pixmap.height());
  const auto data = pixmap.data();

  int minX = w;
  int minY = h;
  int maxX = -1;
  int maxY = -1;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::uint8_t alpha = data[static_cast<std::size_t>((y * w + x) * 4 + 3)];
      if (alpha == 0) {
        continue;
      }

      minX = std::min(minX, x);
      minY = std::min(minY, y);
      maxX = std::max(maxX, x + 1);
      maxY = std::max(maxY, y + 1);
    }
  }

  if (maxX <= minX || maxY <= minY) {
    return std::nullopt;
  }

  return Box{static_cast<double>(minX), static_cast<double>(minY), static_cast<double>(maxX),
             static_cast<double>(maxY)};
}

/// Apply subregion clipping on uint8 pixmap: clear pixels outside the given rect.
void applySubregionClipping(Pixmap& output, const PixelRect& sr, int w, int h) {
  const int rx0 = std::max(0, static_cast<int>(std::floor(sr.x)));
  const int ry0 = std::max(0, static_cast<int>(std::floor(sr.y)));
  const int rx1 = std::clamp(static_cast<int>(std::ceil(sr.x + sr.w)), 0, w);
  const int ry1 = std::clamp(static_cast<int>(std::ceil(sr.y + sr.h)), 0, h);

  auto data = output.data();
  for (int y = 0; y < ry0; ++y) {
    std::fill_n(data.data() + y * w * 4, w * 4, static_cast<std::uint8_t>(0));
  }
  for (int y = ry0; y < ry1; ++y) {
    if (rx0 > 0) {
      std::fill_n(data.data() + y * w * 4, rx0 * 4, static_cast<std::uint8_t>(0));
    }
    if (rx1 < w) {
      std::fill_n(data.data() + (y * w + rx1) * 4, (w - rx1) * 4, static_cast<std::uint8_t>(0));
    }
  }
  for (int y = ry1; y < h; ++y) {
    std::fill_n(data.data() + y * w * 4, w * 4, static_cast<std::uint8_t>(0));
  }
}

/// sRGB to linear conversion for a single channel value in [0,1].
double srgbToLinearChannel(double s) {
  if (s <= 0.04045) {
    return s / 12.92;
  }
  return std::pow((s + 0.055) / 1.055, 2.4);
}

}  // namespace

bool executeFilterGraph(Pixmap& sourceGraphic, const FilterGraph& graph) {
  const int w = static_cast<int>(sourceGraphic.width());
  const int h = static_cast<int>(sourceGraphic.height());
  if (w <= 0 || h <= 0 || graph.nodes.empty()) {
    return false;
  }

  // Hybrid uint8/float approach (matching Skia's raster pipeline). All buffers stored as
  // uint8 sRGB between nodes. When linearRGB processing is needed, temporarily convert to
  // float linear for the filter math to avoid uint8 quantization "black crush" in dark colors.
  Pixmap sourcePixmap = copyPixmap(sourceGraphic);
  std::optional<Pixmap> fillPaintStorage = graph.fillPaintInput;
  std::optional<Pixmap> strokePaintStorage = graph.strokePaintInput;

  if (graph.clipSourceToFilterRegion && graph.filterRegion.has_value()) {
    applySubregionClipping(sourcePixmap, *graph.filterRegion, w, h);
  }

  // Buffer management — all intermediate storage in uint8 sRGB.
  Pixmap* source = &sourcePixmap;
  Pixmap* fillPaint = fillPaintStorage.has_value() ? &*fillPaintStorage : nullptr;
  Pixmap* strokePaint = strokePaintStorage.has_value() ? &*strokePaintStorage : nullptr;
  std::optional<Pixmap> transparentPaintInput;
  std::optional<Pixmap> sourceAlpha;
  std::optional<Pixmap> previousOutput;
  std::map<std::string, Pixmap> namedBuffers;

  // Subregion tracking.
  const Box fullRegion = Box::fromWH(w, h);
  const Box filterRegionBox =
      graph.filterRegion.has_value() ? Box::fromPixelRect(*graph.filterRegion) : fullRegion;
  Box previousOutputSubregion = fullRegion;
  const std::optional<Box> fillPaintSubregion =
      fillPaint != nullptr ? computeNonTransparentBounds(*fillPaint) : std::nullopt;
  const std::optional<Box> strokePaintSubregion =
      strokePaint != nullptr ? computeNonTransparentBounds(*strokePaint) : std::nullopt;
  std::map<std::string, Box> namedSubregions;

  auto getTransparentPaintInput = [&]() -> Pixmap* {
    if (!transparentPaintInput.has_value()) {
      transparentPaintInput = createTransparent(w, h);
    }
    return &*transparentPaintInput;
  };

  auto getSourceAlpha = [&]() -> Pixmap* {
    if (!sourceAlpha.has_value()) {
      sourceAlpha = copyPixmap(*source);
      auto data = sourceAlpha->data();
      for (int i = 0; i < w * h; ++i) {
        data[i * 4 + 0] = 0;
        data[i * 4 + 1] = 0;
        data[i * 4 + 2] = 0;
      }
    }
    return &sourceAlpha.value();
  };

  auto resolveInput = [&](const NodeInput& input) -> Pixmap* {
    return std::visit(
        [&](const auto& v) -> Pixmap* {
          using V = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<V, NodeInput::Previous>) {
            return previousOutput.has_value() ? &previousOutput.value() : source;
          } else if constexpr (std::is_same_v<V, StandardInput>) {
            if (v == StandardInput::SourceGraphic) {
              return source;
            }
            if (v == StandardInput::SourceAlpha) {
              return getSourceAlpha();
            }
            if (v == StandardInput::FillPaint) {
              return fillPaint != nullptr ? fillPaint : getTransparentPaintInput();
            }
            if (v == StandardInput::StrokePaint) {
              return strokePaint != nullptr ? strokePaint : getTransparentPaintInput();
            }
            return source;
          } else if constexpr (std::is_same_v<V, NodeInput::Named>) {
            auto it = namedBuffers.find(v.name);
            if (it != namedBuffers.end()) {
              return &it->second;
            }
            return previousOutput.has_value() ? &previousOutput.value() : source;
          } else {
            return source;
          }
        },
        input.value);
  };

  auto resolveInputSubregion = [&](const NodeInput& input) -> Box {
    return std::visit(
        [&](const auto& v) -> Box {
          using V = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<V, NodeInput::Previous>) {
            return previousOutputSubregion;
          } else if constexpr (std::is_same_v<V, NodeInput::Named>) {
            auto it = namedSubregions.find(v.name);
            if (it != namedSubregions.end()) {
              return it->second;
            }
            return previousOutputSubregion;
          } else if constexpr (std::is_same_v<V, StandardInput>) {
            if (v == StandardInput::FillPaint) {
              return fillPaintSubregion.value_or(fullRegion);
            }
            if (v == StandardInput::StrokePaint) {
              return strokePaintSubregion.value_or(fullRegion);
            }
            return fullRegion;
          } else {
            return fullRegion;
          }
        },
        input.value);
  };

  auto defaultNodeSubregion = [&](const GraphNode& node) -> Box {
    const bool isSourceGenerator =
        std::holds_alternative<graph_primitive::Flood>(node.primitive) ||
        std::holds_alternative<graph_primitive::Turbulence>(node.primitive) ||
        std::holds_alternative<graph_primitive::Image>(node.primitive) ||
        std::holds_alternative<graph_primitive::Tile>(node.primitive);

    if (node.subregion.has_value()) {
      return Box::fromPixelRect(*node.subregion).intersect(filterRegionBox);
    }

    if (node.inputs.empty() || isSourceGenerator) {
      return filterRegionBox;
    }

    Box inputBounds = resolveInputSubregion(node.inputs[0]);
    for (std::size_t i = 1; i < node.inputs.size(); ++i) {
      inputBounds = inputBounds.unite(resolveInputSubregion(node.inputs[i]));
    }

    return std::visit(
               [&](const auto& primitive) -> Box {
                 using T = std::decay_t<decltype(primitive)>;

                 if constexpr (std::is_same_v<T, graph_primitive::GaussianBlur>) {
                   const double expandX = std::ceil(primitive.sigmaX * 3.0);
                   const double expandY = std::ceil(primitive.sigmaY * 3.0);
                   return inputBounds.outset(expandX, expandY);
                 } else if constexpr (std::is_same_v<T, graph_primitive::DropShadow>) {
                   const double expandX = std::ceil(primitive.sigmaX * 3.0);
                   const double expandY = std::ceil(primitive.sigmaY * 3.0);
                   const Box shadowBounds = inputBounds
                                                .translate(static_cast<double>(primitive.dx),
                                                           static_cast<double>(primitive.dy))
                                                .outset(expandX, expandY);
                   return inputBounds.unite(shadowBounds);
                 } else if constexpr (std::is_same_v<T, graph_primitive::Morphology>) {
                   if (primitive.op == MorphologyOp::Dilate) {
                     return inputBounds.outset(static_cast<double>(primitive.radiusX),
                                               static_cast<double>(primitive.radiusY));
                   }

                   return inputBounds;
                 } else {
                   return inputBounds;
                 }
               },
               node.primitive)
        .intersect(filterRegionBox);
  };

  for (const GraphNode& node : graph.nodes) {
    const bool nodeLinearRGB = node.useLinearRGB.value_or(graph.useLinearRGB);

    // All stored buffers are uint8 sRGB. No color space conversion at input resolution.
    Pixmap* input = node.inputs.empty() ? source : resolveInput(node.inputs[0]);

    std::optional<Pixmap> output;

    std::visit(
        [&](const auto& primitive) {
          using T = std::decay_t<decltype(primitive)>;
          using namespace graph_primitive;

          if constexpr (std::is_same_v<T, GaussianBlur>) {
            if (nodeLinearRGB) {
              auto fp = pixmapToLinearFloat(*input);
              gaussianBlur(fp, primitive.sigmaX, primitive.sigmaY, primitive.edgeMode);
              output = linearFloatToPixmap(fp);
            } else {
              output = copyPixmap(*input);
              gaussianBlur(*output, primitive.sigmaX, primitive.sigmaY, primitive.edgeMode);
            }

          } else if constexpr (std::is_same_v<T, Flood>) {
            // Flood color is sRGB uint8. Store as sRGB uint8 directly.
            // Downstream linearRGB nodes convert to float linear when they read.
            output = createTransparent(w, h);
            flood(*output, primitive.r, primitive.g, primitive.b, primitive.a);

          } else if constexpr (std::is_same_v<T, graph_primitive::Offset>) {
            // Pure pixel mover — always uint8, color space doesn't affect the operation.
            output = createTransparent(w, h);
            filter::offset(*input, *output, primitive.dx, primitive.dy);

          } else if constexpr (std::is_same_v<T, graph_primitive::Composite>) {
            Pixmap* input2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            if (nodeLinearRGB) {
              auto fpIn1 = pixmapToLinearFloat(*input);
              auto fpIn2 = pixmapToLinearFloat(*input2);
              auto fpOut =
                  *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                         static_cast<std::uint32_t>(h));
              composite(fpIn1, fpIn2, fpOut, primitive.op, primitive.k1, primitive.k2,
                        primitive.k3, primitive.k4);
              output = linearFloatToPixmap(fpOut);
            } else {
              output = createTransparent(w, h);
              composite(*input, *input2, *output, primitive.op, primitive.k1, primitive.k2,
                        primitive.k3, primitive.k4);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Blend>) {
            Pixmap* input2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            if (nodeLinearRGB) {
              auto fpIn1 = pixmapToLinearFloat(*input);
              auto fpIn2 = pixmapToLinearFloat(*input2);
              auto fpOut =
                  *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                         static_cast<std::uint32_t>(h));
              blend(fpIn2, fpIn1, fpOut, primitive.mode);
              output = linearFloatToPixmap(fpOut);
            } else {
              output = createTransparent(w, h);
              blend(*input2, *input, *output, primitive.mode);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Merge>) {
            if (nodeLinearRGB) {
              std::vector<FloatPixmap> mergeFloats;
              mergeFloats.reserve(node.inputs.size());
              for (const auto& mergeInput : node.inputs) {
                Pixmap* mergeRaw = resolveInput(mergeInput);
                mergeFloats.push_back(pixmapToLinearFloat(*mergeRaw));
              }
              std::vector<const FloatPixmap*> floatLayers;
              floatLayers.reserve(mergeFloats.size());
              for (auto& fp : mergeFloats) {
                floatLayers.push_back(&fp);
              }
              auto fpOut =
                  *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                         static_cast<std::uint32_t>(h));
              merge(std::span<const FloatPixmap* const>(floatLayers), fpOut);
              output = linearFloatToPixmap(fpOut);
            } else {
              std::vector<const Pixmap*> layers;
              for (const auto& mergeInput : node.inputs) {
                layers.push_back(resolveInput(mergeInput));
              }
              output = createTransparent(w, h);
              merge(layers, *output);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::ColorMatrix>) {
            if (primitive.matrix == identityMatrix()) {
              // Identity matrix: skip color space conversion to avoid unnecessary
              // round-trip quantization artifacts. Pass through raw input.
              output = copyPixmap(*input);
            } else if (nodeLinearRGB) {
              auto fp = pixmapToLinearFloat(*input);
              colorMatrix(fp, primitive.matrix);
              output = linearFloatToPixmap(fp);
            } else {
              output = copyPixmap(*input);
              colorMatrix(*output, primitive.matrix);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::ComponentTransfer>) {
            auto toFunc = [](const graph_primitive::ComponentTransfer::Func& f) {
              TransferFunc tf;
              tf.type = f.type;
              tf.tableValues = f.tableValues;
              tf.slope = f.slope;
              tf.intercept = f.intercept;
              tf.amplitude = f.amplitude;
              tf.exponent = f.exponent;
              tf.offset = f.offset;
              return tf;
            };
            if (nodeLinearRGB) {
              auto fp = pixmapToLinearFloat(*input);
              componentTransfer(fp, toFunc(primitive.funcR), toFunc(primitive.funcG),
                                toFunc(primitive.funcB), toFunc(primitive.funcA));
              output = linearFloatToPixmap(fp);
            } else {
              output = copyPixmap(*input);
              componentTransfer(*output, toFunc(primitive.funcR), toFunc(primitive.funcG),
                                toFunc(primitive.funcB), toFunc(primitive.funcA));
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::ConvolveMatrix>) {
            output = createTransparent(w, h);
            const int requiredSize = primitive.orderX * primitive.orderY;
            if (primitive.orderX > 0 && primitive.orderY > 0 &&
                static_cast<int>(primitive.kernel.size()) == requiredSize &&
                primitive.targetX >= 0 && primitive.targetX < primitive.orderX &&
                primitive.targetY >= 0 && primitive.targetY < primitive.orderY &&
                primitive.divisor != 0.0) {
              ConvolveParams params;
              params.orderX = primitive.orderX;
              params.orderY = primitive.orderY;
              params.kernel = primitive.kernel;
              params.divisor = primitive.divisor;
              params.bias = primitive.bias;
              params.targetX = primitive.targetX;
              params.targetY = primitive.targetY;
              params.edgeMode = primitive.edgeMode;
              params.preserveAlpha = primitive.preserveAlpha;
              if (nodeLinearRGB) {
                auto fpIn = pixmapToLinearFloat(*input);
                auto fpOut =
                    *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                           static_cast<std::uint32_t>(h));
                convolveMatrix(fpIn, fpOut, params);
                output = linearFloatToPixmap(fpOut);
              } else {
                convolveMatrix(*input, *output, params);
              }
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Morphology>) {
            output = createTransparent(w, h);
            if (primitive.radiusX >= 0 && primitive.radiusY >= 0 &&
                (primitive.radiusX > 0 || primitive.radiusY > 0)) {
              if (nodeLinearRGB) {
                auto fpIn = pixmapToLinearFloat(*input);
                auto fpOut =
                    *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                           static_cast<std::uint32_t>(h));
                morphology(fpIn, fpOut, primitive.op, primitive.radiusX, primitive.radiusY);
                output = linearFloatToPixmap(fpOut);
              } else {
                morphology(*input, *output, primitive.op, primitive.radiusX, primitive.radiusY);
              }
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Tile>) {
            // Pure pixel mover — always uint8, color space doesn't affect the operation.
            output = createTransparent(w, h);
            const Box inputSubregion = node.inputs.empty() ? previousOutputSubregion
                                                           : resolveInputSubregion(node.inputs[0]);
            const int tileX = std::max(0, static_cast<int>(std::floor(inputSubregion.x0)));
            const int tileY = std::max(0, static_cast<int>(std::floor(inputSubregion.y0)));
            const int tileR = std::min(w, static_cast<int>(std::ceil(inputSubregion.x1)));
            const int tileB = std::min(h, static_cast<int>(std::ceil(inputSubregion.y1)));
            const int tileW = tileR - tileX;
            const int tileH = tileB - tileY;
            if (tileW > 0 && tileH > 0) {
              tile(*input, *output, tileX, tileY, tileW, tileH);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Turbulence>) {
            if (nodeLinearRGB) {
              // Turbulence generates noise values in the processing color space (linear).
              // Convert linear→sRGB for storage.
              auto fp = *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                               static_cast<std::uint32_t>(h));
              turbulence(fp, primitive.params);
              output = linearFloatToPixmap(fp);
            } else {
              output = createTransparent(w, h);
              turbulence(*output, primitive.params);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::DisplacementMap>) {
            Pixmap* input2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            if (nodeLinearRGB) {
              auto fpIn1 = pixmapToLinearFloat(*input);
              auto fpIn2 = pixmapToLinearFloat(*input2);
              auto fpOut =
                  *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                         static_cast<std::uint32_t>(h));
              displacementMap(fpIn1, fpIn2, fpOut, primitive.scale, primitive.xChannel,
                              primitive.yChannel);
              output = linearFloatToPixmap(fpOut);
            } else {
              output = createTransparent(w, h);
              displacementMap(*input, *input2, *output, primitive.scale, primitive.xChannel,
                              primitive.yChannel);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::DiffuseLighting>) {
            if (nodeLinearRGB) {
              auto fpIn = pixmapToLinearFloat(*input);
              auto fpOut =
                  *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                         static_cast<std::uint32_t>(h));
              auto params = primitive.params;
              params.lightR = srgbToLinearChannel(params.lightR);
              params.lightG = srgbToLinearChannel(params.lightG);
              params.lightB = srgbToLinearChannel(params.lightB);
              diffuseLighting(fpIn, fpOut, params);
              output = linearFloatToPixmap(fpOut);
            } else {
              output = createTransparent(w, h);
              diffuseLighting(*input, *output, primitive.params);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::SpecularLighting>) {
            // Per SVG spec, specularExponent must be in [1, 128].
            // Values < 1: produce transparent output. Values > 128: clamp to 128.
            output = createTransparent(w, h);
            if (primitive.params.specularExponent >= 1.0) {
              auto params = primitive.params;
              params.specularExponent = std::min(params.specularExponent, 128.0);
              if (nodeLinearRGB) {
                auto fpIn = pixmapToLinearFloat(*input);
                auto fpOut =
                    *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                           static_cast<std::uint32_t>(h));
                params.lightR = srgbToLinearChannel(params.lightR);
                params.lightG = srgbToLinearChannel(params.lightG);
                params.lightB = srgbToLinearChannel(params.lightB);
                specularLighting(fpIn, fpOut, params);
                output = linearFloatToPixmap(fpOut);
              } else {
                specularLighting(*input, *output, params);
              }
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::DropShadow>) {
            if (nodeLinearRGB) {
              // All sub-operations in float linear.
              auto fpFlood =
                  *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                         static_cast<std::uint32_t>(h));
              flood(fpFlood, primitive.r / 255.0f, primitive.g / 255.0f, primitive.b / 255.0f,
                    primitive.a / 255.0f);
              srgbToLinear(fpFlood);

              // Source alpha as float (RGB zeroed, alpha preserved).
              auto fpSrcAlpha = FloatPixmap::fromPixmap(*source);
              auto srcAlphaData = fpSrcAlpha.data();
              for (int i = 0; i < w * h; ++i) {
                srcAlphaData[i * 4 + 0] = 0.0f;
                srcAlphaData[i * 4 + 1] = 0.0f;
                srcAlphaData[i * 4 + 2] = 0.0f;
              }

              auto fpComposite =
                  *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                         static_cast<std::uint32_t>(h));
              composite(fpFlood, fpSrcAlpha, fpComposite, CompositeOp::In);

              auto fpOffset =
                  *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                         static_cast<std::uint32_t>(h));
              filter::offset(fpComposite, fpOffset, primitive.dx, primitive.dy);

              gaussianBlur(fpOffset, primitive.sigmaX, primitive.sigmaY);

              auto fpInput = pixmapToLinearFloat(*input);
              auto fpOut =
                  *FloatPixmap::fromSize(static_cast<std::uint32_t>(w),
                                         static_cast<std::uint32_t>(h));
              std::vector<const FloatPixmap*> layers = {&fpOffset, &fpInput};
              merge(std::span<const FloatPixmap* const>(layers), fpOut);

              output = linearFloatToPixmap(fpOut);
            } else {
              auto floodBuf = createTransparent(w, h);
              flood(floodBuf, primitive.r, primitive.g, primitive.b, primitive.a);

              auto compositeBuf = createTransparent(w, h);
              Pixmap* srcAlpha = getSourceAlpha();
              composite(floodBuf, *srcAlpha, compositeBuf, CompositeOp::In);

              auto offsetBuf = createTransparent(w, h);
              filter::offset(compositeBuf, offsetBuf, primitive.dx, primitive.dy);

              gaussianBlur(offsetBuf, primitive.sigmaX, primitive.sigmaY);

              output = createTransparent(w, h);
              std::vector<const Pixmap*> layers = {&offsetBuf, input};
              merge(layers, *output);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Image>) {
            // Image data is sRGB. Store as sRGB uint8 directly.
            // Downstream linearRGB nodes convert to float linear when they read.
            output = createTransparent(w, h);
            if (!primitive.pixels.empty() && primitive.width > 0 && primitive.height > 0) {
              const double tx = primitive.targetRect.has_value() ? primitive.targetRect->x : 0.0;
              const double ty = primitive.targetRect.has_value() ? primitive.targetRect->y : 0.0;
              const double tw = primitive.targetRect.has_value() ? primitive.targetRect->w
                                                                 : static_cast<double>(w);
              const double th = primitive.targetRect.has_value() ? primitive.targetRect->h
                                                                 : static_cast<double>(h);

              const double invScaleX = static_cast<double>(primitive.width) / tw;
              const double invScaleY = static_cast<double>(primitive.height) / th;

              auto dstData = output->data();
              const auto& srcData = primitive.pixels;
              const int srcW = primitive.width;
              const int srcH = primitive.height;

              // Bilinear interpolation to scale source image into target rect.
              auto sampleSrc = [&](int sx, int sy, int ch) -> int {
                sx = std::clamp(sx, 0, srcW - 1);
                sy = std::clamp(sy, 0, srcH - 1);
                return srcData[static_cast<std::size_t>((sy * srcW + sx) * 4 + ch)];
              };

              for (int dy = 0; dy < h; ++dy) {
                for (int dx = 0; dx < w; ++dx) {
                  const double srcXf = (static_cast<double>(dx) + 0.5 - tx) * invScaleX - 0.5;
                  const double srcYf = (static_cast<double>(dy) + 0.5 - ty) * invScaleY - 0.5;

                  const int sx0 = static_cast<int>(std::floor(srcXf));
                  const int sy0 = static_cast<int>(std::floor(srcYf));

                  if (sx0 + 1 < 0 || sx0 >= srcW || sy0 + 1 < 0 || sy0 >= srcH) {
                    continue;
                  }

                  const double fx = srcXf - sx0;
                  const double fy = srcYf - sy0;

                  const std::size_t dstIdx = static_cast<std::size_t>((dy * w + dx) * 4);
                  for (int ch = 0; ch < 4; ++ch) {
                    const double s00 = sampleSrc(sx0, sy0, ch);
                    const double s10 = sampleSrc(sx0 + 1, sy0, ch);
                    const double s01 = sampleSrc(sx0, sy0 + 1, ch);
                    const double s11 = sampleSrc(sx0 + 1, sy0 + 1, ch);
                    const double top = s00 + (s10 - s00) * fx;
                    const double bottom = s01 + (s11 - s01) * fx;
                    dstData[dstIdx + ch] = static_cast<std::uint8_t>(
                        std::clamp(std::lround(top + (bottom - top) * fy), 0L, 255L));
                  }
                }
              }
            }
          }
        },
        node.primitive);

    if (output.has_value()) {
      const Box nodeSubregion = defaultNodeSubregion(node);

      // Clip output pixels to the computed subregion.
      const PixelRect clipRect{nodeSubregion.x0, nodeSubregion.y0,
                               nodeSubregion.x1 - nodeSubregion.x0,
                               nodeSubregion.y1 - nodeSubregion.y0};
      applySubregionClipping(*output, clipRect, w, h);

      if (node.result.has_value()) {
        namedBuffers[*node.result] = copyPixmap(*output);
        namedSubregions[*node.result] = nodeSubregion;
      }
      previousOutput = std::move(output);
      previousOutputSubregion = nodeSubregion;
    }
  }

  // All stored data is already sRGB — no final color space conversion needed.
  if (previousOutput.has_value()) {
    auto srcData = previousOutput->data();
    auto dstData = sourceGraphic.data();
    std::copy(srcData.begin(), srcData.end(), dstData.begin());
    return true;
  }
  return false;
}

}  // namespace tiny_skia::filter
