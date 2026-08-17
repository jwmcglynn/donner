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
/// When filterFromDevice and userSpaceSubregion are provided, uses per-pixel point-in-rect
/// testing for rotation-aware clipping instead of the axis-aligned bounding box.
void applySubregionClipping(FloatPixmap& output, const PixelRect& sr, int w, int h,
                            const AffineTransform* filterFromDevice = nullptr,
                            const PixelRect* userSpaceSubregion = nullptr) {
  // Rotation-aware path: transform each pixel center to user space and test against the
  // user-space subregion rectangle.
  if (filterFromDevice != nullptr && userSpaceSubregion != nullptr) {
    const auto& t = *filterFromDevice;
    const auto& usr = *userSpaceSubregion;
    const double ux0 = usr.x;
    const double uy0 = usr.y;
    const double ux1 = usr.x + usr.w;
    const double uy1 = usr.y + usr.h;

    auto data = output.data();
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const double px = x + 0.5;
        const double py = y + 0.5;
        const double fx = t.a * px + t.c * py + t.e;
        const double fy = t.b * px + t.d * py + t.f;
        if (fx < ux0 || fx >= ux1 || fy < uy0 || fy >= uy1) {
          const auto idx = static_cast<std::size_t>((y * w + x) * 4);
          data[idx + 0] = 0.0f;
          data[idx + 1] = 0.0f;
          data[idx + 2] = 0.0f;
          data[idx + 3] = 0.0f;
        }
      }
    }
    return;
  }

  // Axis-aligned fast path. Clamp the kept-rect origin to the pixmap bounds as well as the far
  // edge: clamping rx0/ry0 only at the low end (max(0, ...)) leaves them able to exceed w/h when
  // the subregion maps entirely past the right/bottom edge, and the per-row "clear the left border
  // [0, rx0)" fill would then write rx0*4 bytes into a w*4-byte row and walk past the buffer.
  // Clamping to [0, w]/[0, h] keeps every fill in bounds; a fully-outside subregion collapses to an
  // empty kept rect (rx0 == rx1 or ry0 == ry1) so the whole pixmap is cleared.
  const int rx0 = std::clamp(static_cast<int>(std::floor(sr.x)), 0, w);
  const int ry0 = std::clamp(static_cast<int>(std::floor(sr.y)), 0, h);
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

/// sRGB to linear conversion for a single channel value in [0,1].
double srgbToLinearChannel(double s) {
  if (s <= 0.04045) {
    return s / 12.92;
  }
  return std::pow((s + 0.055) / 1.055, 2.4);
}

/// Pixels a node just produced, tagged with the color space they are expressed in.
struct NodeOutput {
  FloatPixmap pixmap;
  /// true when the channel values are linearRGB, false when they are sRGB.
  bool linear = false;
  /// true when the two spaces hold bit-identical values for this buffer, which is the case
  /// exactly when every RGB channel is zero: unpremultiplying zero yields zero, both transfer
  /// functions map zero to zero, and neither touches alpha. Alpha-only buffers therefore never
  /// need a conversion pass, no matter which space consumes them.
  bool spaceInvariant = false;
};

/// A filter buffer that remembers which color space its pixels are in.
///
/// Primitives are defined to operate in an interpolation space, and SVG picks that space per
/// primitive rather than per graph (`color-interpolation-filters`). Tagging each buffer instead
/// of normalizing every node's result back to one storage space means a graph whose primitives
/// agree on a space converts once on the way in and once on the way out, while a mixed graph pays
/// a conversion only on the edges that actually cross between spaces.
///
/// The stored pixels are never converted in place, because one buffer can feed several nodes:
/// an in-place conversion would either corrupt a later consumer that wants the original space or
/// force a lossy round trip back to it. The other space is materialized alongside instead, and
/// only once per buffer.
class SpacedPixmap {
 public:
  SpacedPixmap() = default;

  SpacedPixmap(FloatPixmap pixmap, bool linear) : pixmap_(std::move(pixmap)), linear_(linear) {}

  explicit SpacedPixmap(NodeOutput output)
      : pixmap_(std::move(output.pixmap)),
        linear_(output.linear),
        spaceInvariant_(output.spaceInvariant) {}

  /// Wraps a buffer whose RGB channels are all zero, which reads identically in both spaces.
  static SpacedPixmap alphaOnly(FloatPixmap pixmap) {
    SpacedPixmap result(std::move(pixmap), /*linear=*/false);
    result.spaceInvariant_ = true;
    return result;
  }

  /// Returns the pixels in the requested space, materializing and caching the conversion the
  /// first time a consumer asks for a space the stored pixels are not already in.
  const FloatPixmap& in(bool linear) {
    if (spaceInvariant_ || linear == linear_) {
      return pixmap_;
    }

    if (!converted_.has_value()) {
      FloatPixmap fp(pixmap_);
      if (linear) {
        srgbToLinear(fp);
      } else {
        linearToSrgb(fp);
      }
      converted_ = std::move(fp);
    }

    return *converted_;
  }

  /// Returns the stored pixels without converting. Only valid for operations that are
  /// independent of the color space, such as moving or clearing pixels.
  [[nodiscard]] const FloatPixmap& spaceAgnostic() const { return pixmap_; }

  /// Takes ownership of the pixels in the requested space, converting in place when no cached
  /// conversion exists. The buffer must not be read afterwards.
  FloatPixmap release(bool linear) {
    if (spaceInvariant_ || linear == linear_) {
      return std::move(pixmap_);
    }

    if (converted_.has_value()) {
      return std::move(*converted_);
    }

    FloatPixmap fp = std::move(pixmap_);
    if (linear) {
      srgbToLinear(fp);
    } else {
      linearToSrgb(fp);
    }
    return fp;
  }

  /// Describes the stored pixels so a space-independent operation can tag its own result the
  /// same way.
  [[nodiscard]] NodeOutput describe(FloatPixmap pixmap) const {
    return NodeOutput{std::move(pixmap), linear_, spaceInvariant_};
  }

 private:
  FloatPixmap pixmap_;
  bool linear_ = false;
  bool spaceInvariant_ = false;
  std::optional<FloatPixmap> converted_;
};

}  // namespace

bool executeFilterGraph(Pixmap& sourceGraphic, const FilterGraph& graph) {
  const int w = static_cast<int>(sourceGraphic.width());
  const int h = static_cast<int>(sourceGraphic.height());
  if (w <= 0 || h <= 0 || graph.nodes.empty()) {
    return false;
  }

  // Float intermediate storage avoids uint8 quantization between nodes. Every buffer is
  // float [0,1] premultiplied, tagged with the color space it holds (see SpacedPixmap). The
  // graph's pixel data therefore crosses between sRGB and linearRGB only where two adjacent
  // primitives disagree about the interpolation space, instead of on every primitive: the usual
  // graph, where all primitives share one space, converts once on entry and once on exit.
  FloatPixmap sourceFloatPixels = FloatPixmap::fromPixmap(sourceGraphic);

  if (graph.clipSourceToFilterRegion && graph.filterRegion.has_value()) {
    if (graph.filterFromDevice.has_value() && graph.userSpaceFilterRegion.has_value()) {
      applySubregionClipping(sourceFloatPixels, *graph.filterRegion, w, h, &*graph.filterFromDevice,
                             &*graph.userSpaceFilterRegion);
    } else {
      applySubregionClipping(sourceFloatPixels, *graph.filterRegion, w, h);
    }
  }

  // The source graphic and the paint inputs arrive as uint8 sRGB.
  SpacedPixmap sourceFloat(std::move(sourceFloatPixels), /*linear=*/false);
  std::optional<SpacedPixmap> fillPaintStorage =
      graph.fillPaintInput.has_value()
          ? std::make_optional(SpacedPixmap(FloatPixmap::fromPixmap(*graph.fillPaintInput),
                                            /*linear=*/false))
          : std::nullopt;
  std::optional<SpacedPixmap> strokePaintStorage =
      graph.strokePaintInput.has_value()
          ? std::make_optional(SpacedPixmap(FloatPixmap::fromPixmap(*graph.strokePaintInput),
                                            /*linear=*/false))
          : std::nullopt;

  // Buffer management.
  SpacedPixmap* source = &sourceFloat;
  SpacedPixmap* fillPaint = fillPaintStorage.has_value() ? &*fillPaintStorage : nullptr;
  SpacedPixmap* strokePaint = strokePaintStorage.has_value() ? &*strokePaintStorage : nullptr;
  std::optional<SpacedPixmap> transparentPaintInput;
  std::optional<SpacedPixmap> sourceAlpha;
  std::optional<SpacedPixmap> previousOutput;
  std::map<std::string, SpacedPixmap> namedBuffers;

  // Subregion tracking.
  const Box fullRegion = Box::fromWH(w, h);
  const Box filterRegionBox =
      graph.filterRegion.has_value() ? Box::fromPixelRect(*graph.filterRegion) : fullRegion;
  Box previousOutputSubregion = fullRegion;
  // Alpha coverage does not depend on the color space, so the paint bounds can read the stored
  // pixels directly.
  const std::optional<Box> fillPaintSubregion =
      fillPaint != nullptr ? computeNonTransparentBounds(fillPaint->spaceAgnostic()) : std::nullopt;
  const std::optional<Box> strokePaintSubregion =
      strokePaint != nullptr ? computeNonTransparentBounds(strokePaint->spaceAgnostic())
                             : std::nullopt;
  std::map<std::string, Box> namedSubregions;

  auto getTransparentPaintInput = [&]() -> SpacedPixmap* {
    if (!transparentPaintInput.has_value()) {
      transparentPaintInput = SpacedPixmap::alphaOnly(createTransparentFloat(w, h));
    }
    return &*transparentPaintInput;
  };

  auto getSourceAlpha = [&]() -> SpacedPixmap* {
    if (!sourceAlpha.has_value()) {
      // Zeroing RGB makes the buffer read the same in either space, so SourceAlpha never needs a
      // conversion pass regardless of which space the consuming primitive works in.
      FloatPixmap alphaOnly(source->spaceAgnostic());
      auto data = alphaOnly.data();
      for (int i = 0; i < w * h; ++i) {
        data[i * 4 + 0] = 0.0f;
        data[i * 4 + 1] = 0.0f;
        data[i * 4 + 2] = 0.0f;
      }
      sourceAlpha = SpacedPixmap::alphaOnly(std::move(alphaOnly));
    }
    return &sourceAlpha.value();
  };

  auto resolveInput = [&](const NodeInput& input) -> SpacedPixmap* {
    return std::visit(
        [&](const auto& v) -> SpacedPixmap* {
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

    // Each buffer knows its own color space, so a node asks its inputs for the space it works in
    // and tags its result with that same space. `in()` is a no-op whenever the producer already
    // agreed with this node, which is the common case.
    SpacedPixmap* input = node.inputs.empty() ? source : resolveInput(node.inputs[0]);

    std::optional<NodeOutput> output;

    std::visit(
        [&](const auto& primitive) {
          using T = std::decay_t<decltype(primitive)>;
          using namespace graph_primitive;

          if constexpr (std::is_same_v<T, GaussianBlur>) {
            auto fp = FloatPixmap(input->in(nodeLinearRGB));
            gaussianBlur(fp, primitive.sigmaX, primitive.sigmaY, primitive.edgeMode);
            output = NodeOutput{std::move(fp), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, Flood>) {
            // Flood color is premultiplied sRGB uint8, so a linearRGB node has to convert it into
            // its own space.
            auto fp = createTransparentFloat(w, h);
            flood(fp, primitive.r / 255.0f, primitive.g / 255.0f, primitive.b / 255.0f,
                  primitive.a / 255.0f);
            if (nodeLinearRGB) {
              srgbToLinear(fp);
            }
            output = NodeOutput{std::move(fp), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, graph_primitive::Offset>) {
            // Pure pixel mover: the result is the input's pixels in the input's space, so it
            // needs no conversion in either direction.
            auto fpOut = createTransparentFloat(w, h);
            filter::offset(input->spaceAgnostic(), fpOut, primitive.dx, primitive.dy);
            output = input->describe(std::move(fpOut));

          } else if constexpr (std::is_same_v<T, graph_primitive::Composite>) {
            SpacedPixmap* input2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            const FloatPixmap& in1 = input->in(nodeLinearRGB);
            const FloatPixmap& in2 = input2->in(nodeLinearRGB);
            auto fpOut = createTransparentFloat(w, h);
            composite(in1, in2, fpOut, primitive.op, primitive.k1, primitive.k2, primitive.k3,
                      primitive.k4);
            output = NodeOutput{std::move(fpOut), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, graph_primitive::Blend>) {
            SpacedPixmap* input2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            const FloatPixmap& in1 = input->in(nodeLinearRGB);
            const FloatPixmap& in2 = input2->in(nodeLinearRGB);
            auto fpOut = createTransparentFloat(w, h);
            blend(in2, in1, fpOut, primitive.mode);
            output = NodeOutput{std::move(fpOut), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, graph_primitive::Merge>) {
            std::vector<const FloatPixmap*> layers;
            layers.reserve(node.inputs.size());
            for (const auto& mergeInput : node.inputs) {
              layers.push_back(&resolveInput(mergeInput)->in(nodeLinearRGB));
            }
            auto fpOut = createTransparentFloat(w, h);
            merge(std::span<const FloatPixmap* const>(layers), fpOut);
            output = NodeOutput{std::move(fpOut), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, graph_primitive::ColorMatrix>) {
            if (primitive.matrix == identityMatrix()) {
              // Identity matrix: pass through, which is independent of the color space.
              output = input->describe(FloatPixmap(input->spaceAgnostic()));
            } else {
              auto fp = FloatPixmap(input->in(nodeLinearRGB));
              colorMatrix(fp, primitive.matrix);
              output = NodeOutput{std::move(fp), nodeLinearRGB};
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
            auto fp = FloatPixmap(input->in(nodeLinearRGB));
            componentTransfer(fp, toFunc(primitive.funcR), toFunc(primitive.funcG),
                              toFunc(primitive.funcB), toFunc(primitive.funcA));
            output = NodeOutput{std::move(fp), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, graph_primitive::ConvolveMatrix>) {
            auto fpOut = createTransparentFloat(w, h);
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
              convolveMatrix(input->in(nodeLinearRGB), fpOut, params);
            }
            output = NodeOutput{std::move(fpOut), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, graph_primitive::Morphology>) {
            // SVG Filter Effects §15.4: a negative radius, or a zero radius on both axes
            // (which is also the lacuna value 0 used for an absent/empty/invalid `radius`),
            // disables the effect — the result is the filter input image (pass-through), not
            // transparent black. A zero radius on only one axis is a no-op along that axis,
            // so the effect still applies (e.g. radius="10 0" erodes horizontally only).
            const bool disabled = primitive.radiusX < 0 || primitive.radiusY < 0 ||
                                  (primitive.radiusX == 0 && primitive.radiusY == 0);
            if (disabled) {
              // Pass-through, which is independent of the color space.
              output = input->describe(FloatPixmap(input->spaceAgnostic()));
            } else {
              auto fpOut = createTransparentFloat(w, h);
              morphology(input->in(nodeLinearRGB), fpOut, primitive.op, primitive.radiusX,
                         primitive.radiusY);
              output = NodeOutput{std::move(fpOut), nodeLinearRGB};
            }

          } else if constexpr (std::is_same_v<T, graph_primitive::Tile>) {
            // Pure pixel mover, so the result stays in the input's space with no conversion.
            auto fpOut = createTransparentFloat(w, h);
            const Box inputSubregion = node.inputs.empty() ? previousOutputSubregion
                                                           : resolveInputSubregion(node.inputs[0]);
            const int tileX = std::max(0, static_cast<int>(std::floor(inputSubregion.x0)));
            const int tileY = std::max(0, static_cast<int>(std::floor(inputSubregion.y0)));
            const int tileR = std::min(w, static_cast<int>(std::ceil(inputSubregion.x1)));
            const int tileB = std::min(h, static_cast<int>(std::ceil(inputSubregion.y1)));
            const int tileW = tileR - tileX;
            const int tileH = tileB - tileY;
            if (tileW > 0 && tileH > 0) {
              tile(input->spaceAgnostic(), fpOut, tileX, tileY, tileW, tileH);
            }
            output = input->describe(std::move(fpOut));

          } else if constexpr (std::is_same_v<T, graph_primitive::Turbulence>) {
            // Turbulence generates noise directly in the node's interpolation space.
            auto fp = createTransparentFloat(w, h);
            turbulence(fp, primitive.params);
            output = NodeOutput{std::move(fp), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, graph_primitive::DisplacementMap>) {
            SpacedPixmap* input2 = node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : source;
            const FloatPixmap& in1 = input->in(nodeLinearRGB);
            const FloatPixmap& in2 = input2->in(nodeLinearRGB);
            auto fpOut = createTransparentFloat(w, h);
            displacementMap(in1, in2, fpOut, primitive.scale, primitive.xChannel,
                            primitive.yChannel);
            output = NodeOutput{std::move(fpOut), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, graph_primitive::DiffuseLighting>) {
            auto fpOut = createTransparentFloat(w, h);
            auto params = primitive.params;
            if (nodeLinearRGB) {
              // The light color is authored in sRGB, so it follows the pixels into linearRGB.
              params.lightR = srgbToLinearChannel(params.lightR);
              params.lightG = srgbToLinearChannel(params.lightG);
              params.lightB = srgbToLinearChannel(params.lightB);
            }
            diffuseLighting(input->in(nodeLinearRGB), fpOut, params);
            output = NodeOutput{std::move(fpOut), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, graph_primitive::SpecularLighting>) {
            // Per SVG spec, specularExponent must be in [1, 128].
            // Values < 1: produce transparent output. Values > 128: clamp to 128.
            auto fpOut = createTransparentFloat(w, h);
            if (primitive.params.specularExponent >= 1.0) {
              auto params = primitive.params;
              params.specularExponent = std::min(params.specularExponent, 128.0);
              if (nodeLinearRGB) {
                params.lightR = srgbToLinearChannel(params.lightR);
                params.lightG = srgbToLinearChannel(params.lightG);
                params.lightB = srgbToLinearChannel(params.lightB);
              }
              specularLighting(input->in(nodeLinearRGB), fpOut, params);
            }
            output = NodeOutput{std::move(fpOut), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, graph_primitive::DropShadow>) {
            // Decomposed into flood + composite-in + offset + blur + merge, all run in the
            // node's own interpolation space. Only the flood color needs converting, because it
            // is authored as premultiplied sRGB; the source alpha carries no color.
            auto floodBuf = createTransparentFloat(w, h);
            flood(floodBuf, primitive.r / 255.0f, primitive.g / 255.0f, primitive.b / 255.0f,
                  primitive.a / 255.0f);
            if (nodeLinearRGB) {
              srgbToLinear(floodBuf);
            }

            auto compositeBuf = createTransparentFloat(w, h);
            composite(floodBuf, getSourceAlpha()->spaceAgnostic(), compositeBuf, CompositeOp::In);

            auto offsetBuf = createTransparentFloat(w, h);
            filter::offset(compositeBuf, offsetBuf, primitive.dx, primitive.dy);

            gaussianBlur(offsetBuf, primitive.sigmaX, primitive.sigmaY);

            auto fpOut = createTransparentFloat(w, h);
            const std::vector<const FloatPixmap*> layers = {&offsetBuf, &input->in(nodeLinearRGB)};
            merge(std::span<const FloatPixmap* const>(layers), fpOut);
            output = NodeOutput{std::move(fpOut), nodeLinearRGB};

          } else if constexpr (std::is_same_v<T, graph_primitive::Image>) {
            // Image data is sRGB uint8, and the resampling below runs on those sRGB values, so
            // the result is tagged sRGB no matter which space this node interpolates in. A
            // linearRGB consumer converts it, exactly as it would convert any other sRGB buffer.
            //
            // Mitchell-Netravali bicubic resampling in float.
            //
            // resvg (the reference renderer) resamples feImage content with the
            // Mitchell-Netravali cubic kernel (B = C = 1/3), the standard "high quality"
            // downscale/upscale filter. A plain bilinear kernel produces a measurably
            // different interpolation ramp when a small image is heavily upscaled (the
            // bilinear ramp is monotone, while the Mitchell kernel mildly overshoots near
            // hard color edges), which is exactly what the resvg goldens encode. Matching the
            // kernel removes the upscale-ramp pixel diffs the goldens flag (verified to be a
            // bit-for-bit match against resvg's feImage subregion goldens).
            auto fpOut = createTransparentFloat(w, h);
            if (!primitive.pixels.empty() && primitive.width > 0 && primitive.height > 0) {
              const double tx = primitive.targetRect.has_value() ? primitive.targetRect->x : 0.0;
              const double ty = primitive.targetRect.has_value() ? primitive.targetRect->y : 0.0;
              const double tw = primitive.targetRect.has_value() ? primitive.targetRect->w
                                                                 : static_cast<double>(w);
              const double th = primitive.targetRect.has_value() ? primitive.targetRect->h
                                                                 : static_cast<double>(h);

              const double invScaleX = static_cast<double>(primitive.width) / tw;
              const double invScaleY = static_cast<double>(primitive.height) / th;

              auto dstData = fpOut.data();
              const auto& srcData = primitive.pixels;
              const int srcW = primitive.width;
              const int srcH = primitive.height;

              // Read a source channel with edge clamping, sRGB uint8 → [0,1] float.
              auto sampleSrc = [&](int sx, int sy, int ch) -> double {
                sx = std::clamp(sx, 0, srcW - 1);
                sy = std::clamp(sy, 0, srcH - 1);
                return srcData[static_cast<std::size_t>((sy * srcW + sx) * 4 + ch)] / 255.0;
              };

              // Mitchell-Netravali cubic weighting function (B = C = 1/3).
              constexpr double kB = 1.0 / 3.0;
              constexpr double kC = 1.0 / 3.0;
              auto cubicWeight = [&](double t) -> double {
                t = std::abs(t);
                if (t < 1.0) {
                  return ((12.0 - 9.0 * kB - 6.0 * kC) * t * t * t +
                          (-18.0 + 12.0 * kB + 6.0 * kC) * t * t + (6.0 - 2.0 * kB)) /
                         6.0;
                }
                if (t < 2.0) {
                  return ((-kB - 6.0 * kC) * t * t * t + (6.0 * kB + 30.0 * kC) * t * t +
                          (-12.0 * kB - 48.0 * kC) * t + (8.0 * kB + 24.0 * kC)) /
                         6.0;
                }
                return 0.0;
              };

              for (int dy = 0; dy < h; ++dy) {
                for (int dx = 0; dx < w; ++dx) {
                  const double pixelCenterX = static_cast<double>(dx) + 0.5;
                  const double pixelCenterY = static_cast<double>(dy) + 0.5;
                  if (pixelCenterX < tx || pixelCenterX >= tx + tw || pixelCenterY < ty ||
                      pixelCenterY >= ty + th) {
                    continue;
                  }

                  const double srcXf = (static_cast<double>(dx) + 0.5 - tx) * invScaleX - 0.5;
                  const double srcYf = (static_cast<double>(dy) + 0.5 - ty) * invScaleY - 0.5;

                  // `image-rendering: pixelated`/`crisp-edges` on the source feImage: sample the
                  // single nearest texel (the texel whose [i, i+1) span contains the source
                  // position `srcXf + 0.5`) instead of the bicubic footprint. Exact per-texel, so
                  // there is no interpolation ramp; block edges stay hard.
                  if (primitive.pixelated) {
                    const int nsx =
                        std::clamp(static_cast<int>(std::floor(srcXf + 0.5)), 0, srcW - 1);
                    const int nsy =
                        std::clamp(static_cast<int>(std::floor(srcYf + 0.5)), 0, srcH - 1);
                    const std::size_t dstIdxN = static_cast<std::size_t>((dy * w + dx) * 4);
                    const float aN = static_cast<float>(sampleSrc(nsx, nsy, 3));
                    dstData[dstIdxN + 0] = std::min(static_cast<float>(sampleSrc(nsx, nsy, 0)), aN);
                    dstData[dstIdxN + 1] = std::min(static_cast<float>(sampleSrc(nsx, nsy, 1)), aN);
                    dstData[dstIdxN + 2] = std::min(static_cast<float>(sampleSrc(nsx, nsy, 2)), aN);
                    dstData[dstIdxN + 3] = aN;
                    continue;
                  }

                  const int sx0 = static_cast<int>(std::floor(srcXf));
                  const int sy0 = static_cast<int>(std::floor(srcYf));

                  // Precompute the separable 4x4 weights (taps n,m in [-1, 2]).
                  double wx[4];
                  double wy[4];
                  for (int n = -1; n <= 2; ++n) {
                    wx[n + 1] = cubicWeight(srcXf - static_cast<double>(sx0 + n));
                    wy[n + 1] = cubicWeight(srcYf - static_cast<double>(sy0 + n));
                  }

                  const std::size_t dstIdx = static_cast<std::size_t>((dy * w + dx) * 4);
                  float out[4];
                  for (int ch = 0; ch < 4; ++ch) {
                    double acc = 0.0;
                    for (int m = -1; m <= 2; ++m) {
                      double rowAcc = 0.0;
                      for (int n = -1; n <= 2; ++n) {
                        rowAcc += sampleSrc(sx0 + n, sy0 + m, ch) * wx[n + 1];
                      }
                      acc += rowAcc * wy[m + 1];
                    }
                    out[ch] = std::clamp(static_cast<float>(acc), 0.0f, 1.0f);
                  }
                  // Mitchell's negative lobes can drive a premultiplied input's R/G/B above
                  // its A on edges; clamp R/G/B to A so downstream filter primitives — which
                  // consume `FloatPixmap` as premultiplied — don't see ghost-bright halos
                  // (PR #610 Codex P1). For straight-alpha inputs this is a no-op since the
                  // earlier bilinear path stored unclamped values that already satisfied
                  // R/G/B ≤ A on the suite's tests.
                  const float a = out[3];
                  dstData[dstIdx + 0] = std::min(out[0], a);
                  dstData[dstIdx + 1] = std::min(out[1], a);
                  dstData[dstIdx + 2] = std::min(out[2], a);
                  dstData[dstIdx + 3] = a;
                }
              }
            }
            output = NodeOutput{std::move(fpOut), /*linear=*/false};
          }
        },
        node.primitive);

    if (output.has_value()) {
      const Box nodeSubregion = defaultNodeSubregion(node);

      // Clip output pixels to the computed subregion.
      const PixelRect clipRect{nodeSubregion.x0, nodeSubregion.y0,
                               nodeSubregion.x1 - nodeSubregion.x0,
                               nodeSubregion.y1 - nodeSubregion.y0};
      const AffineTransform* xform =
          (graph.filterFromDevice.has_value() && node.userSpaceSubregion.has_value())
              ? &*graph.filterFromDevice
              : nullptr;
      const PixelRect* usrSub = xform ? &*node.userSpaceSubregion : nullptr;
      // Clearing pixels is the same operation in either space, so it runs before the result is
      // published and no conversion is involved.
      applySubregionClipping(output->pixmap, clipRect, w, h, xform, usrSub);

      SpacedPixmap produced(std::move(*output));
      if (node.result.has_value()) {
        namedBuffers[*node.result] = produced;
        namedSubregions[*node.result] = nodeSubregion;
      }
      previousOutput = std::move(produced);
      previousOutputSubregion = nodeSubregion;
    }
  }

  // The graph's result leaves in sRGB: this is the single exit conversion, and it is skipped
  // entirely when the last node already worked in sRGB.
  if (previousOutput.has_value()) {
    Pixmap result = previousOutput->release(/*linear=*/false).toPixmap();
    auto srcData = result.data();
    auto dstData = sourceGraphic.data();
    std::copy(srcData.begin(), srcData.end(), dstData.begin());
    return true;
  }
  return false;
}

}  // namespace tiny_skia::filter
