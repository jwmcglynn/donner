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

  static Box fromPixelRect(const PixelRect& r) {
    return {r.x, r.y, r.x + r.w, r.y + r.h};
  }

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
};

Pixmap createTransparentPixmap(int w, int h) {
  if (w <= 0 || h <= 0) {
    return Pixmap();
  }
  auto maybePixmap =
      Pixmap::fromSize(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
  if (!maybePixmap.has_value()) {
    return Pixmap();
  }
  maybePixmap->fill(Color::transparent);
  return std::move(*maybePixmap);
}

/// Apply subregion clipping: clear pixels outside the given pixel-space rect.
void applySubregionClipping(Pixmap& output, const PixelRect& sr, int w, int h) {
  const int rx0 = std::max(0, static_cast<int>(std::floor(sr.x)));
  const int ry0 = std::max(0, static_cast<int>(std::floor(sr.y)));
  const int rx1 = std::min(w, static_cast<int>(std::ceil(sr.x + sr.w)));
  const int ry1 = std::min(h, static_cast<int>(std::ceil(sr.y + sr.h)));

  auto data = output.data();
  for (int y = 0; y < ry0; ++y) {
    std::fill_n(data.data() + y * w * 4, w * 4, std::uint8_t{0});
  }
  for (int y = ry0; y < ry1; ++y) {
    if (rx0 > 0) {
      std::fill_n(data.data() + y * w * 4, rx0 * 4, std::uint8_t{0});
    }
    if (rx1 < w) {
      std::fill_n(data.data() + (y * w + rx1) * 4, (w - rx1) * 4, std::uint8_t{0});
    }
  }
  for (int y = ry1; y < h; ++y) {
    std::fill_n(data.data() + y * w * 4, w * 4, std::uint8_t{0});
  }
}

}  // namespace

bool executeFilterGraph(Pixmap& sourceGraphic, const FilterGraph& graph) {
  const int w = static_cast<int>(sourceGraphic.width());
  const int h = static_cast<int>(sourceGraphic.height());
  if (w <= 0 || h <= 0 || graph.nodes.empty()) {
    return false;
  }

  // Convert SourceGraphic from sRGB to linearRGB for filter processing.
  if (graph.useLinearRGB) {
    srgbToLinear(sourceGraphic);
  }

  // Buffer management.
  Pixmap* source = &sourceGraphic;
  std::optional<Pixmap> sourceAlpha;
  std::optional<Pixmap> previousOutput;
  std::map<std::string, Pixmap> namedBuffers;

  // Subregion tracking.
  const Box fullRegion = Box::fromWH(w, h);
  const Box filterRegionBox =
      graph.filterRegion.has_value() ? Box::fromPixelRect(*graph.filterRegion) : fullRegion;
  Box previousOutputSubregion = fullRegion;
  std::map<std::string, Box> namedSubregions;

  // Lazily create SourceAlpha: same as SourceGraphic but with RGB=0, keeping only alpha.
  auto getSourceAlpha = [&]() -> Pixmap* {
    if (!sourceAlpha.has_value()) {
      sourceAlpha = *source;
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
            // FillPaint, StrokePaint not yet implemented.
            return source;
          } else if constexpr (std::is_same_v<V, NodeInput::Named>) {
            auto it = namedBuffers.find(v.name);
            if (it != namedBuffers.end()) {
              return &it->second;
            }
            return source;
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
            return fullRegion;
          } else {
            return fullRegion;
          }
        },
        input.value);
  };

  for (const GraphNode& node : graph.nodes) {
    Pixmap* input = node.inputs.empty() ? source : resolveInput(node.inputs[0]);

    std::optional<Pixmap> output;

    std::visit(
        [&](const auto& primitive) {
          using T = std::decay_t<decltype(primitive)>;
          using namespace graph_primitive;

          if constexpr (std::is_same_v<T, GaussianBlur>) {
            output = *input;
            gaussianBlur(*output, primitive.sigmaX, primitive.sigmaY, primitive.edgeMode);

          } else if constexpr (std::is_same_v<T, Flood>) {
            output = createTransparentPixmap(w, h);
            flood(*output, primitive.r, primitive.g, primitive.b, primitive.a);
            if (graph.useLinearRGB) {
              srgbToLinear(*output);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Offset>) {
            output = createTransparentPixmap(w, h);
            filter::offset(*input, *output, primitive.dx, primitive.dy);

          } else if constexpr (std::is_same_v<T, graph_primitive::Composite>) {
            Pixmap* input2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            output = createTransparentPixmap(w, h);
            composite(*input, *input2, *output, primitive.op, primitive.k1, primitive.k2,
                      primitive.k3, primitive.k4);

          } else if constexpr (std::is_same_v<T, graph_primitive::Blend>) {
            Pixmap* input2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            output = createTransparentPixmap(w, h);
            // SVG: in=foreground (Cs), in2=background (Cb).
            blend(*input2, *input, *output, primitive.mode);

          } else if constexpr (std::is_same_v<T, graph_primitive::Merge>) {
            std::vector<const Pixmap*> layers;
            for (const auto& mergeInput : node.inputs) {
              layers.push_back(resolveInput(mergeInput));
            }
            output = createTransparentPixmap(w, h);
            merge(layers, *output);

          } else if constexpr (std::is_same_v<T, graph_primitive::ColorMatrix>) {
            output = *input;
            colorMatrix(*output, primitive.matrix);

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
            output = createTransparentPixmap(w, h);
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
            output = createTransparentPixmap(w, h);
            if (primitive.radiusX >= 0 && primitive.radiusY >= 0 &&
                (primitive.radiusX > 0 || primitive.radiusY > 0)) {
              morphology(*input, *output, primitive.op, primitive.radiusX, primitive.radiusY);
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Tile>) {
            output = createTransparentPixmap(w, h);
            const Box inputSubregion = node.inputs.empty()
                                           ? previousOutputSubregion
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
            output = createTransparentPixmap(w, h);
            turbulence(*output, primitive.params);

          } else if constexpr (std::is_same_v<T, graph_primitive::DisplacementMap>) {
            Pixmap* input2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            output = createTransparentPixmap(w, h);
            displacementMap(*input, *input2, *output, primitive.scale, primitive.xChannel,
                            primitive.yChannel);

          } else if constexpr (std::is_same_v<T, graph_primitive::DiffuseLighting>) {
            output = createTransparentPixmap(w, h);
            diffuseLighting(*input, *output, primitive.params);

          } else if constexpr (std::is_same_v<T, graph_primitive::SpecularLighting>) {
            output = createTransparentPixmap(w, h);
            specularLighting(*input, *output, primitive.params);

          } else if constexpr (std::is_same_v<T, graph_primitive::DropShadow>) {
            // Decompose: flood → composite(in, SourceAlpha) → offset → blur → merge
            auto floodBuf = createTransparentPixmap(w, h);
            flood(floodBuf, primitive.r, primitive.g, primitive.b, primitive.a);
            if (graph.useLinearRGB) {
              srgbToLinear(floodBuf);
            }

            auto compositeBuf = createTransparentPixmap(w, h);
            Pixmap* srcAlpha = getSourceAlpha();
            composite(floodBuf, *srcAlpha, compositeBuf, CompositeOp::In);

            auto offsetBuf = createTransparentPixmap(w, h);
            filter::offset(compositeBuf, offsetBuf, primitive.dx, primitive.dy);

            gaussianBlur(offsetBuf, primitive.sigmaX, primitive.sigmaY);

            output = createTransparentPixmap(w, h);
            std::vector<const Pixmap*> layers = {&offsetBuf, input};
            merge(layers, *output);

          } else if constexpr (std::is_same_v<T, graph_primitive::Image>) {
            output = createTransparentPixmap(w, h);
          }
        },
        node.primitive);

    if (output.has_value()) {
      // Apply subregion clipping.
      if (node.subregion.has_value()) {
        applySubregionClipping(*output, *node.subregion, w, h);
      }

      // Track effective subregion.
      Box nodeSubregion;
      if (node.subregion.has_value()) {
        nodeSubregion = Box::fromPixelRect(*node.subregion);
      } else {
        nodeSubregion = node.inputs.empty() ? previousOutputSubregion
                                            : resolveInputSubregion(node.inputs[0]);
      }
      nodeSubregion = nodeSubregion.intersect(filterRegionBox);

      if (node.result.has_value()) {
        namedBuffers[*node.result] = *output;
        namedSubregions[*node.result] = nodeSubregion;
      }
      previousOutput = std::move(output);
      previousOutputSubregion = nodeSubregion;
    }
  }

  // Convert final output from linearRGB back to sRGB and copy to source pixmap.
  if (previousOutput.has_value()) {
    if (graph.useLinearRGB) {
      linearToSrgb(*previousOutput);
    }
    auto srcData = previousOutput->data();
    auto dstData = sourceGraphic.data();
    std::copy(srcData.begin(), srcData.end(), dstData.begin());
    return true;
  }
  return false;
}

}  // namespace tiny_skia::filter
