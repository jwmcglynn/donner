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
#include "tiny_skia/filter/FloatPixmap.h"
#include "tiny_skia/filter/Flood.h"
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

FloatPixmap createTransparentFloat(int w, int h) {
  auto fp = FloatPixmap::fromSize(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
  if (!fp.has_value()) {
    return FloatPixmap();
  }
  return std::move(*fp);
}

std::optional<Box> computeNonTransparentBounds(const FloatPixmap& pixmap) {
  const int w = static_cast<int>(pixmap.width());
  const int h = static_cast<int>(pixmap.height());
  const auto data = pixmap.data();

  int minX = w;
  int minY = h;
  int maxX = -1;
  int maxY = -1;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float alpha = data[static_cast<std::size_t>((y * w + x) * 4 + 3)];
      if (alpha <= 0.0f) {
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

/// Apply subregion clipping on float pixmap: clear pixels outside the given rect.
void applySubregionClipping(FloatPixmap& output, const PixelRect& sr, int w, int h) {
  const int rx0 = std::max(0, static_cast<int>(std::floor(sr.x)));
  const int ry0 = std::max(0, static_cast<int>(std::floor(sr.y)));
  const int rx1 = std::clamp(static_cast<int>(std::ceil(sr.x + sr.w)), 0, w);
  const int ry1 = std::clamp(static_cast<int>(std::ceil(sr.y + sr.h)), 0, h);

  auto data = output.data();
  for (int y = 0; y < ry0; ++y) {
    std::fill_n(data.data() + y * w * 4, w * 4, 0.0f);
  }
  for (int y = ry0; y < ry1; ++y) {
    if (rx0 > 0) {
      std::fill_n(data.data() + y * w * 4, rx0 * 4, 0.0f);
    }
    if (rx1 < w) {
      std::fill_n(data.data() + (y * w + rx1) * 4, (w - rx1) * 4, 0.0f);
    }
  }
  for (int y = ry1; y < h; ++y) {
    std::fill_n(data.data() + y * w * 4, w * 4, 0.0f);
  }
}

}  // namespace

bool executeFilterGraph(Pixmap& sourceGraphic, const FilterGraph& graph) {
  const int w = static_cast<int>(sourceGraphic.width());
  const int h = static_cast<int>(sourceGraphic.height());
  if (w <= 0 || h <= 0 || graph.nodes.empty()) {
    return false;
  }

  // Convert source to float. Keep in sRGB — convert to linear only when needed per-node.
  // This deferred conversion approach allows identity operations (like identity feColorMatrix)
  // to avoid the sRGB↔linear round-trip entirely, eliminating quantization artifacts.
  FloatPixmap sourceFloat = FloatPixmap::fromPixmap(sourceGraphic);
  std::optional<FloatPixmap> fillPaintStorage = graph.fillPaintInput;
  std::optional<FloatPixmap> strokePaintStorage = graph.strokePaintInput;

  if (graph.clipSourceToFilterRegion && graph.filterRegion.has_value()) {
    applySubregionClipping(sourceFloat, *graph.filterRegion, w, h);
  }

  // Buffer management — all intermediate work in float precision.
  FloatPixmap* source = &sourceFloat;
  FloatPixmap* fillPaint = fillPaintStorage.has_value() ? &*fillPaintStorage : nullptr;
  FloatPixmap* strokePaint = strokePaintStorage.has_value() ? &*strokePaintStorage : nullptr;
  std::optional<FloatPixmap> transparentPaintInput;
  std::optional<FloatPixmap> sourceAlpha;
  std::optional<FloatPixmap> previousOutput;
  std::map<std::string, FloatPixmap> namedBuffers;

  // sRGB to linear conversion for a single channel value in [0,1].
  auto srgbToLinearChannel = [](double s) -> double {
    if (s <= 0.04045) {
      return s / 12.92;
    }
    return std::pow((s + 0.055) / 1.055, 2.4);
  };

  // Color space tracking: true = linearRGB, false = sRGB.
  // Source starts in sRGB; conversion to linear is done per-node via ensureColorSpace.
  bool sourceColorSpace = false;
  const bool fillPaintColorSpace = false;
  const bool strokePaintColorSpace = false;
  bool previousOutputColorSpace = false;
  std::map<std::string, bool> namedColorSpaces;

  auto ensureColorSpace = [](FloatPixmap* buf, bool currentIsLinear, bool targetIsLinear,
                             std::optional<FloatPixmap>& tempBuf) -> FloatPixmap* {
    if (currentIsLinear == targetIsLinear) {
      return buf;
    }
    tempBuf = *buf;
    if (targetIsLinear) {
      srgbToLinear(*tempBuf);
    } else {
      linearToSrgb(*tempBuf);
    }
    return &*tempBuf;
  };

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

  auto getTransparentPaintInput = [&]() -> FloatPixmap* {
    if (!transparentPaintInput.has_value()) {
      transparentPaintInput = createTransparentFloat(w, h);
    }
    return &*transparentPaintInput;
  };

  auto getSourceAlpha = [&]() -> FloatPixmap* {
    if (!sourceAlpha.has_value()) {
      sourceAlpha = *source;
      auto data = sourceAlpha->data();
      for (int i = 0; i < w * h; ++i) {
        data[i * 4 + 0] = 0.0f;
        data[i * 4 + 1] = 0.0f;
        data[i * 4 + 2] = 0.0f;
      }
    }
    return &sourceAlpha.value();
  };

  auto resolveInput = [&](const NodeInput& input) -> FloatPixmap* {
    return std::visit(
        [&](const auto& v) -> FloatPixmap* {
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
            // Per SVG spec: "If no matching filter primitive has been previously defined [with
            // this result name], the attribute is treated as if the attribute is not specified."
            // When unspecified, non-first primitives use the previous result, and the first
            // primitive uses SourceGraphic.
            return previousOutput.has_value() ? &previousOutput.value() : source;
          } else {
            return source;
          }
        },
        input.value);
  };

  auto resolveInputColorSpace = [&](const NodeInput& input) -> bool {
    return std::visit(
        [&](const auto& v) -> bool {
          using V = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<V, NodeInput::Previous>) {
            return previousOutputColorSpace;
          } else if constexpr (std::is_same_v<V, NodeInput::Named>) {
            auto it = namedColorSpaces.find(v.name);
            if (it != namedColorSpaces.end()) {
              return it->second;
            }
            // Invalid name: treat as if `in` was not specified (previous result fallback).
            return previousOutputColorSpace;
          } else if constexpr (std::is_same_v<V, StandardInput>) {
            if (v == StandardInput::FillPaint) {
              return fillPaintColorSpace;
            }
            if (v == StandardInput::StrokePaint) {
              return strokePaintColorSpace;
            }
            return sourceColorSpace;
          } else {
            return sourceColorSpace;
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
            // Invalid name: treat as if `in` was not specified (previous result fallback).
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

    FloatPixmap* rawInput = node.inputs.empty() ? source : resolveInput(node.inputs[0]);
    bool inputCS = node.inputs.empty() ? sourceColorSpace : resolveInputColorSpace(node.inputs[0]);

    std::optional<FloatPixmap> convertedInput;
    FloatPixmap* input = ensureColorSpace(rawInput, inputCS, nodeLinearRGB, convertedInput);

    // Color space of this node's output — defaults to nodeLinearRGB but may be overridden
    // by specific primitives (e.g., identity feColorMatrix skips color space conversion).
    bool outputColorSpace = nodeLinearRGB;

    std::optional<FloatPixmap> output;

    std::visit(
        [&](const auto& primitive) {
          using T = std::decay_t<decltype(primitive)>;
          using namespace graph_primitive;

          if constexpr (std::is_same_v<T, GaussianBlur>) {
            output = *input;
            gaussianBlur(*output, primitive.sigmaX, primitive.sigmaY, primitive.edgeMode);

          } else if constexpr (std::is_same_v<T, Flood>) {
            output = createTransparentFloat(w, h);
            // Flood color is sRGB uint8 — convert to float [0,1].
            float r = primitive.r / 255.0f;
            float g = primitive.g / 255.0f;
            float b = primitive.b / 255.0f;
            float a = primitive.a / 255.0f;
            flood(*output, r, g, b, a);
            if (nodeLinearRGB) {
              srgbToLinear(*output);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Offset>) {
            output = createTransparentFloat(w, h);
            filter::offset(*input, *output, primitive.dx, primitive.dy);

          } else if constexpr (std::is_same_v<T, graph_primitive::Composite>) {
            FloatPixmap* rawInput2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            bool input2CS =
                node.inputs.size() > 1 ? resolveInputColorSpace(node.inputs[1]) : sourceColorSpace;
            std::optional<FloatPixmap> convertedInput2;
            FloatPixmap* input2 =
                ensureColorSpace(rawInput2, input2CS, nodeLinearRGB, convertedInput2);
            output = createTransparentFloat(w, h);
            composite(*input, *input2, *output, primitive.op, primitive.k1, primitive.k2,
                      primitive.k3, primitive.k4);

          } else if constexpr (std::is_same_v<T, graph_primitive::Blend>) {
            FloatPixmap* rawInput2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            bool input2CS =
                node.inputs.size() > 1 ? resolveInputColorSpace(node.inputs[1]) : sourceColorSpace;
            std::optional<FloatPixmap> convertedInput2;
            FloatPixmap* input2 =
                ensureColorSpace(rawInput2, input2CS, nodeLinearRGB, convertedInput2);
            output = createTransparentFloat(w, h);
            blend(*input2, *input, *output, primitive.mode);

          } else if constexpr (std::is_same_v<T, graph_primitive::Merge>) {
            std::vector<FloatPixmap> mergeConverted;
            std::vector<const FloatPixmap*> layers;
            for (const auto& mergeInput : node.inputs) {
              FloatPixmap* mergeRaw = resolveInput(mergeInput);
              bool mergeCS = resolveInputColorSpace(mergeInput);
              if (mergeCS != nodeLinearRGB) {
                mergeConverted.push_back(*mergeRaw);
                if (nodeLinearRGB) {
                  srgbToLinear(mergeConverted.back());
                } else {
                  linearToSrgb(mergeConverted.back());
                }
                layers.push_back(&mergeConverted.back());
              } else {
                layers.push_back(mergeRaw);
              }
            }
            output = createTransparentFloat(w, h);
            merge(layers, *output);

          } else if constexpr (std::is_same_v<T, graph_primitive::ColorMatrix>) {
            if (primitive.matrix == identityMatrix()) {
              // Identity matrix: skip color space conversion to avoid unnecessary
              // sRGB↔linear round-trip quantization artifacts. Pass through raw input.
              output = *rawInput;
              outputColorSpace = inputCS;
            } else {
              output = *input;
              colorMatrix(*output, primitive.matrix);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::ComponentTransfer>) {
            output = *input;
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
            componentTransfer(*output, toFunc(primitive.funcR), toFunc(primitive.funcG),
                              toFunc(primitive.funcB), toFunc(primitive.funcA));

          } else if constexpr (std::is_same_v<T, graph_primitive::ConvolveMatrix>) {
            output = createTransparentFloat(w, h);
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
              convolveMatrix(*input, *output, params);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Morphology>) {
            output = createTransparentFloat(w, h);
            if (primitive.radiusX >= 0 && primitive.radiusY >= 0 &&
                (primitive.radiusX > 0 || primitive.radiusY > 0)) {
              morphology(*input, *output, primitive.op, primitive.radiusX, primitive.radiusY);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Tile>) {
            output = createTransparentFloat(w, h);
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
            output = createTransparentFloat(w, h);
            turbulence(*output, primitive.params);

          } else if constexpr (std::is_same_v<T, graph_primitive::DisplacementMap>) {
            FloatPixmap* rawInput2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            bool input2CS =
                node.inputs.size() > 1 ? resolveInputColorSpace(node.inputs[1]) : sourceColorSpace;
            std::optional<FloatPixmap> convertedInput2;
            FloatPixmap* input2 =
                ensureColorSpace(rawInput2, input2CS, nodeLinearRGB, convertedInput2);
            output = createTransparentFloat(w, h);
            displacementMap(*input, *input2, *output, primitive.scale, primitive.xChannel,
                            primitive.yChannel);

          } else if constexpr (std::is_same_v<T, graph_primitive::DiffuseLighting>) {
            output = createTransparentFloat(w, h);
            if (nodeLinearRGB) {
              // Convert lighting-color from sRGB to linear for the linearRGB pipeline.
              auto params = primitive.params;
              params.lightR = srgbToLinearChannel(params.lightR);
              params.lightG = srgbToLinearChannel(params.lightG);
              params.lightB = srgbToLinearChannel(params.lightB);
              diffuseLighting(*input, *output, params);
            } else {
              diffuseLighting(*input, *output, primitive.params);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::SpecularLighting>) {
            output = createTransparentFloat(w, h);
            // Per SVG spec, specularExponent must be in [1, 128].
            // Values < 1: skip the primitive entirely (produce transparent output).
            // Values > 128: clamp to 128 and render normally.
            // This matches resvg behavior.
            if (primitive.params.specularExponent >= 1.0) {
              auto params = primitive.params;
              params.specularExponent = std::min(params.specularExponent, 128.0);
              if (nodeLinearRGB) {
                params.lightR = srgbToLinearChannel(params.lightR);
                params.lightG = srgbToLinearChannel(params.lightG);
                params.lightB = srgbToLinearChannel(params.lightB);
              }
              specularLighting(*input, *output, params);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::DropShadow>) {
            // Decompose: flood → composite(in, SourceAlpha) → offset → blur → merge
            auto floodBuf = createTransparentFloat(w, h);
            float fr = primitive.r / 255.0f;
            float fg = primitive.g / 255.0f;
            float fb = primitive.b / 255.0f;
            float fa = primitive.a / 255.0f;
            flood(floodBuf, fr, fg, fb, fa);
            if (nodeLinearRGB) {
              srgbToLinear(floodBuf);
            }

            auto compositeBuf = createTransparentFloat(w, h);
            FloatPixmap* srcAlpha = getSourceAlpha();
            composite(floodBuf, *srcAlpha, compositeBuf, CompositeOp::In);

            auto offsetBuf = createTransparentFloat(w, h);
            filter::offset(compositeBuf, offsetBuf, primitive.dx, primitive.dy);

            gaussianBlur(offsetBuf, primitive.sigmaX, primitive.sigmaY);

            output = createTransparentFloat(w, h);
            std::vector<const FloatPixmap*> layers = {&offsetBuf, input};
            merge(layers, *output);

          } else if constexpr (std::is_same_v<T, graph_primitive::Image>) {
            output = createTransparentFloat(w, h);
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
              // Sample source pixel as float [0,1], with clamp edge handling.
              auto sampleSrc = [&](int sx, int sy, int ch) -> float {
                sx = std::clamp(sx, 0, srcW - 1);
                sy = std::clamp(sy, 0, srcH - 1);
                return srcData[static_cast<std::size_t>((sy * srcW + sx) * 4 + ch)] / 255.0f;
              };

              for (int dy = 0; dy < h; ++dy) {
                for (int dx = 0; dx < w; ++dx) {
                  // Map destination pixel center to source coordinates.
                  const double srcXf = (static_cast<double>(dx) + 0.5 - tx) * invScaleX - 0.5;
                  const double srcYf = (static_cast<double>(dy) + 0.5 - ty) * invScaleY - 0.5;

                  const int sx0 = static_cast<int>(std::floor(srcXf));
                  const int sy0 = static_cast<int>(std::floor(srcYf));

                  if (sx0 + 1 < 0 || sx0 >= srcW || sy0 + 1 < 0 || sy0 >= srcH) {
                    continue;
                  }

                  const float fx = static_cast<float>(srcXf - sx0);
                  const float fy = static_cast<float>(srcYf - sy0);

                  const std::size_t dstIdx = static_cast<std::size_t>((dy * w + dx) * 4);
                  for (int ch = 0; ch < 4; ++ch) {
                    const float s00 = sampleSrc(sx0, sy0, ch);
                    const float s10 = sampleSrc(sx0 + 1, sy0, ch);
                    const float s01 = sampleSrc(sx0, sy0 + 1, ch);
                    const float s11 = sampleSrc(sx0 + 1, sy0 + 1, ch);
                    const float top = s00 + (s10 - s00) * fx;
                    const float bottom = s01 + (s11 - s01) * fx;
                    dstData[dstIdx + ch] = top + (bottom - top) * fy;
                  }
                }
              }
              if (nodeLinearRGB) {
                srgbToLinear(*output);
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
        namedBuffers[*node.result] = *output;
        namedSubregions[*node.result] = nodeSubregion;
        namedColorSpaces[*node.result] = outputColorSpace;
      }
      previousOutput = std::move(output);
      previousOutputSubregion = nodeSubregion;
      previousOutputColorSpace = outputColorSpace;
    }
  }

  // Convert final float output back to uint8 sRGB and copy to source pixmap.
  if (previousOutput.has_value()) {
    if (previousOutputColorSpace) {
      linearToSrgb(*previousOutput);
    }
    Pixmap result = previousOutput->toPixmap();
    auto srcData = result.data();
    auto dstData = sourceGraphic.data();
    std::copy(srcData.begin(), srcData.end(), dstData.begin());
    return true;
  }
  return false;
}

}  // namespace tiny_skia::filter
