#pragma once
/// @file

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

#include "donner/base/MathUtils.h"
#include "donner/base/Transform.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/filter/FilterGraph.h"

namespace donner::svg {

/// Clamp a finite nonnegative radius to the filter-work bound. @param value Requested pixels.
inline double BoundedPositiveFilterPixels(double value) {
  if (!std::isfinite(value) || value <= 0.0) {
    return 0.0;
  }
  return std::min(value, static_cast<double>(components::kMaximumFilterPixelRadius));
}

/// Clamp a finite offset to the filter-work bound. @param value Requested pixels.
inline double BoundedSignedFilterPixels(double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::clamp(value, -static_cast<double>(components::kMaximumFilterPixelOffset),
                    static_cast<double>(components::kMaximumFilterPixelOffset));
}

/// The number of inputs a primitive reads through the filter attribute surface. feComposite,
/// feBlend and feDisplacementMap each take a second input (`in2`); feMerge is variadic and reads
/// exactly the list it was built with, and every other primitive reads at most one input. The cap
/// of two is deliberate rather than a lower bound: those primitives never sample a third input, so
/// an extra entry a caller appends must not widen the node's subregion either.
inline size_t FilterInputSlotCount(const components::FilterNode& node) {
  using namespace components;
  if (std::holds_alternative<filter_primitive::Composite>(node.primitive) ||
      std::holds_alternative<filter_primitive::Blend>(node.primitive) ||
      std::holds_alternative<filter_primitive::DisplacementMap>(node.primitive)) {
    return 2;
  }
  return node.inputs.size();
}

/// The input in slot @p index, or the default for a slot the graph left empty. A slot the graph
/// never populated is an unspecified `in`/`in2` attribute, and both resolve the same way: to the
/// preceding primitive's result, which is SourceGraphic only when this node is the first
/// primitive in the filter. Falling back to the node's own first input instead would silently
/// composite, blend, or displace a buffer against itself.
inline components::FilterInput FilterInputOrDefault(const components::FilterNode& node,
                                                    size_t index) {
  return index < node.inputs.size() ? node.inputs[index] : components::FilterInput{};
}

/// Device scale of the filter X axis. @param transform Filter-to-device transform.
inline double FilterDeviceScaleX(const Transform2d& transform) {
  return std::max(transform.transformVector(Vector2d(1.0, 0.0)).length(), 1e-12);
}

/// Device scale of the filter Y axis. @param transform Transform. @param scaleX X-axis scale.
inline double FilterDeviceScaleY(const Transform2d& transform, double scaleX) {
  return NearZero(scaleX, 1e-12) ? std::abs(transform.data[3])
                                 : std::max(std::abs(transform.determinant()) / scaleX, 1e-12);
}

/// Resolve an effect length in device pixels.
/// @param value Length. @param bound Object extent. @param scale Device scale.
/// @param objectBounds Whether unitless lengths use object-bounding-box units.
inline double FilterLengthInPixels(double value, double bound, double scale, bool objectBounds) {
  return std::abs(objectBounds ? value * bound : value) * scale;
}

/// Resolve a filter offset vector in device coordinates.
/// @param offset User offset. @param bounds Object extents. @param objectBounds Unit mode.
/// @param transform Filter-to-device transform.
inline Vector2d FilterOffsetInPixels(Vector2d offset, Vector2d bounds, bool objectBounds,
                                     const Transform2d& transform) {
  const Vector2d userOffset =
      objectBounds ? Vector2d(offset.x * bounds.x, offset.y * bounds.y) : offset;
  return transform.transformVector(userOffset);
}

/// Input a filter layer's capture must include beyond each output pixel, in device pixels.
struct FilterInputMargin {
  Vector2d pixels;                   ///< Farthest input read from an output pixel, per axis.
  std::optional<Box2d> userCapture;  ///< User-space rectangle the capture must cover in full.
  Vector2d captureReach;     ///< Input read beyond \ref userCapture by the primitives feeding it.
  bool wholeRegion = false;  ///< Some primitive can read any position in its input.
};

/// Accumulates each primitive's input reach in device pixels, converting lengths with the same
/// helpers and bounds the filter executors use. The executors round each primitive to whole
/// pixels (offsets and morphology radii to the nearest pixel, blur kernels up), so each
/// primitive's reach rounds up to whole pixels before the chain adds them.
struct FilterInputMarginBuilder {
  Transform2d deviceFromFilter;  ///< Filter user space to raster pixels.
  Vector2d scale;                ///< Device scale of each filter axis.
  Vector2d bounds;               ///< Element bounding box size, or 1 without one.
  bool objectBounds = false;     ///< Whether primitive lengths are bounding-box fractions.
  FilterInputMargin margin;      ///< Accumulated margin.

  /// Device length of a nonnegative primitive length. @param value Length. @param axis 0 or 1.
  double length(double value, int axis) const {
    return value >= 0 ? BoundedPositiveFilterPixels(
                            FilterLengthInPixels(value, axis == 0 ? bounds.x : bounds.y,
                                                 axis == 0 ? scale.x : scale.y, objectBounds))
                      : 0.0;
  }

  /// Round a reach up to whole pixels. @param pixels Reach per axis.
  static Vector2d wholePixels(Vector2d pixels) {
    return Vector2d(std::ceil(pixels.x), std::ceil(pixels.y));
  }

  /// Device footprint of a reach measured in pixels along each filter axis. Under rotation or
  /// skew, the transformed blur path samples along the filter axes, which land on slanted device
  /// directions: each device axis gets the absolute linear coefficients times the user-space
  /// reach. Primitives that execute on the device raster read along the device axes instead, so
  /// the footprint keeps at least the per-axis reach.
  /// @param axisPixels Reach in pixels along the filter X and Y axes.
  Vector2d footprint(Vector2d axisPixels) const {
    const double* m = deviceFromFilter.data;
    const double userX = axisPixels.x / scale.x;
    const double userY = axisPixels.y / scale.y;
    // A slanted reach can exceed the per-axis radius bound; the raster layout bounds it later.
    const double limit = static_cast<double>(components::kMaximumFilterSurfaceDimension);
    const auto device = [&](double reach, double axisReach) {
      return std::isfinite(reach) ? std::clamp(reach, axisReach, std::max(axisReach, limit))
                                  : limit;
    };
    return Vector2d(device(std::abs(m[0]) * userX + std::abs(m[2]) * userY, axisPixels.x),
                    device(std::abs(m[1]) * userX + std::abs(m[3]) * userY, axisPixels.y));
  }

  /// Device extent of a primitive offset. @param dx X offset. @param dy Y offset.
  Vector2d offset(double dx, double dy) const {
    const Vector2d pixels = FilterOffsetInPixels({dx, dy}, bounds, objectBounds, deviceFromFilter);
    return Vector2d(std::abs(BoundedSignedFilterPixels(pixels.x)),
                    std::abs(BoundedSignedFilterPixels(pixels.y)));
  }

  /// Wrapping edges read the opposite side of the input, which may lie anywhere in the region.
  void add(const components::filter_primitive::GaussianBlur& blur) {
    if (blur.edgeMode == components::filter_primitive::GaussianBlur::EdgeMode::Wrap) {
      margin.wholeRegion = true;
      return;
    }
    margin.pixels += footprint(wholePixels(
        Vector2d(3.0 * length(blur.stdDeviationX, 0), 3.0 * length(blur.stdDeviationY, 1))));
  }

  void add(const components::filter_primitive::DropShadow& shadow) {
    margin.pixels += wholePixels(offset(shadow.dx, shadow.dy)) +
                     footprint(wholePixels(Vector2d(3.0 * length(shadow.stdDeviationX, 0),
                                                    3.0 * length(shadow.stdDeviationY, 1))));
  }

  void add(const components::filter_primitive::Offset& value) {
    margin.pixels += wholePixels(offset(value.dx, value.dy));
  }

  void add(const components::filter_primitive::Morphology& morphology) {
    margin.pixels += footprint(
        wholePixels(Vector2d(length(morphology.radiusX, 0), length(morphology.radiusY, 1))));
  }

  /// The backends disagree on how displacement scales: one applies the scale directly in raster
  /// pixels, the other multiplies it by the transform's area scale. Bound both. Both sample
  /// bilinearly, reading one pixel past the rounded-down displaced position.
  void add(const components::filter_primitive::DisplacementMap& displacement) {
    const double* m = deviceFromFilter.data;
    const double transformScale =
        std::max({1.0, std::sqrt(std::abs(deviceFromFilter.determinant())), std::abs(m[0]),
                  std::abs(m[1]), std::abs(m[2]), std::abs(m[3])});
    const double half = std::abs(displacement.scale) * 0.5 *
                        (objectBounds ? std::sqrt(bounds.x * bounds.y) : 1.0) * transformScale;
    const double bounded = std::ceil(BoundedSignedFilterPixels(half)) + 1.0;
    margin.pixels += Vector2d(bounded, bounded);
  }

  /// Wrapping edges read the opposite side of the input, which may lie anywhere in the region.
  void add(const components::filter_primitive::ConvolveMatrix& convolve) {
    if (convolve.edgeMode == components::filter_primitive::ConvolveMatrix::EdgeMode::Wrap) {
      margin.wholeRegion = true;
      return;
    }
    const int maxOrder = components::kMaximumFilterConvolveOrder;
    margin.pixels += footprint(Vector2d(std::clamp(convolve.orderX, 0, maxOrder),
                                        std::clamp(convolve.orderY, 0, maxOrder)));
  }

  /// Surface normals read the neighboring pixels.
  void add(const components::filter_primitive::DiffuseLighting&) {
    margin.pixels += footprint(Vector2d(1.0, 1.0));
  }

  /// Surface normals read the neighboring pixels.
  void add(const components::filter_primitive::SpecularLighting&) {
    margin.pixels += footprint(Vector2d(1.0, 1.0));
  }

  /// Point-wise primitives read only the input pixel under each output pixel.
  template <typename T>
  void add(const T&) {}

  /// feTile repeats its input's subregion, so the capture must cover that whole rectangle. It is
  /// known here only when the input primitive authors its region in user-space units; otherwise
  /// the tile may read anywhere in the filter region.
  void addTile(const components::FilterNode* input) {
    const auto userLength = [](const std::optional<Lengthd>& value) {
      return value.has_value() &&
             (value->unit == Lengthd::Unit::Px || value->unit == Lengthd::Unit::None);
    };
    if (input == nullptr || objectBounds || input->hasAnimatedRegion() || !userLength(input->x) ||
        !userLength(input->y) || !userLength(input->width) || !userLength(input->height)) {
      margin.wholeRegion = true;
      return;
    }
    const Box2d source = Box2d::FromXYWH(input->x->value, input->y->value, input->width->value,
                                         input->height->value);
    margin.userCapture = margin.userCapture ? Box2d::Union(*margin.userCapture, source) : source;
    // The primitives before the tile read past its source subregion.
    margin.captureReach = Vector2d(std::max(margin.captureReach.x, margin.pixels.x),
                                   std::max(margin.captureReach.y, margin.pixels.y));
  }
};

/// The node a primitive's first input names, or `nullptr` when it reads a standard input.
inline const components::FilterNode* FilterFirstInputNode(const components::FilterGraph& graph,
                                                          size_t index) {
  using namespace components;
  const FilterInput input = FilterInputOrDefault(graph.nodes[index], 0);
  if (const auto* name = std::get_if<FilterInput::Named>(&input.value)) {
    for (size_t prior = index; prior-- > 0;) {
      if (graph.nodes[prior].result.has_value() && *graph.nodes[prior].result == name->name) {
        return &graph.nodes[prior];
      }
    }
  }
  if (std::holds_alternative<FilterStandardInput>(input.value) || index == 0) {
    return nullptr;
  }
  return &graph.nodes[index - 1];
}

/// Finish a device reach: one more pixel covers rounding at the reach's edge.
/// @param reach Accumulated device reach per axis.
inline Vector2d FinishFilterReach(Vector2d reach) {
  return reach.x > 0.0 || reach.y > 0.0 ? reach + Vector2d(1.0, 1.0) : reach;
}

/// Conservatively measure how far beyond each output pixel a filter graph reads its raster. Every
/// primitive counts, including those fed only by generators: feFlood, feImage and feTurbulence
/// produce output only over the raster, so a later blur or tile still needs the raster to extend
/// past the viewport. Reaches add up, since chained primitives compound.
/// @param graph Filter graph. @param deviceFromFilter Filter user space to raster pixels.
inline FilterInputMargin ComputeFilterSamplingReach(const components::FilterGraph& graph,
                                                    const Transform2d& deviceFromFilter) {
  const bool objectBounds = graph.primitiveUnits == PrimitiveUnits::ObjectBoundingBox &&
                            graph.elementBoundingBox.has_value();
  const double scaleX = FilterDeviceScaleX(deviceFromFilter);
  FilterInputMarginBuilder builder{
      deviceFromFilter,
      Vector2d(scaleX, FilterDeviceScaleY(deviceFromFilter, scaleX)),
      objectBounds ? graph.elementBoundingBox->size() : Vector2d(1.0, 1.0),
      objectBounds,
      {}};
  for (size_t index = 0; index < graph.nodes.size(); ++index) {
    if (std::holds_alternative<components::filter_primitive::Tile>(graph.nodes[index].primitive)) {
      builder.addTile(FilterFirstInputNode(graph, index));
      continue;
    }
    std::visit([&](const auto& primitive) { builder.add(primitive); },
               graph.nodes[index].primitive);
  }
  FilterInputMargin margin = builder.margin;
  margin.pixels = FinishFilterReach(margin.pixels);
  margin.captureReach = FinishFilterReach(margin.captureReach);
  return margin;
}

/// The input margin a filter layer's capture gets: the graph's sampling reach, or none when the
/// capture must stay viewport-aligned (see
/// \ref components::FilterGraph::keepsViewportAlignedCapture and feTurbulence).
/// @param graph Filter graph. @param deviceFromFilter Filter user space to raster pixels.
inline FilterInputMargin ComputeFilterInputMargin(const components::FilterGraph& graph,
                                                  const Transform2d& deviceFromFilter) {
  if (graph.keepsViewportAlignedCapture || components::FilterGraphHasTurbulence(graph)) {
    return {};
  }
  return ComputeFilterSamplingReach(graph, deviceFromFilter);
}

/// Filter input raster layout around a viewport: the viewport plus input margins on each side.
struct FilterInputBuffer {
  int width = 0;    ///< Raster width in pixels.
  int height = 0;   ///< Raster height in pixels.
  int offsetX = 0;  ///< Raster pixels left of the viewport.
  int offsetY = 0;  ///< Raster pixels above the viewport.

  /// Whether the raster extends past the viewport on any side.
  [[nodiscard]] bool exceedsViewport(int viewportWidth, int viewportHeight) const {
    return offsetX != 0 || offsetY != 0 || width != viewportWidth || height != viewportHeight;
  }
};

/// One axis of a \ref FilterInputBuffer.
struct FilterInputAxis {
  int before = 0;  ///< Pixels before the viewport.
  int extent = 0;  ///< Total pixels.
};

/// Pixels to extend a filter input raster past one viewport edge.
/// @param beyond How far the filter region extends past the edge.
/// @param needed Input the graph reads past that edge.
inline int FilterInputExtension(double beyond, double needed) {
  if (!(beyond > 0.0) || !(needed > 0.0)) {
    return 0;
  }
  const double limit = static_cast<double>(components::kMaximumFilterSurfaceDimension);
  return static_cast<int>(std::ceil(std::min({beyond, needed, limit})));
}

/// Lay out one raster axis.
/// @param region Filter region extent on the axis, in device pixels.
/// @param reach Input read past an output pixel. @param capture Input rectangle to cover.
/// @param viewport Viewport extent on the axis.
inline FilterInputAxis LayoutFilterInputAxis(std::pair<double, double> region, double reach,
                                             std::optional<std::pair<double, double>> capture,
                                             int viewport) {
  const double viewportEnd = static_cast<double>(viewport);
  const double neededBefore = std::max(reach, capture ? -capture->first : 0.0);
  const double neededAfter = std::max(reach, capture ? capture->second - viewportEnd : 0.0);
  const int room = std::max(0, components::kMaximumFilterSurfaceDimension - viewport);
  FilterInputAxis axis;
  axis.before = std::min(FilterInputExtension(-region.first, neededBefore), room);
  const int after =
      std::min(FilterInputExtension(region.second - viewportEnd, neededAfter), room - axis.before);
  axis.extent = viewport + axis.before + after;
  return axis;
}

/// Lay out the input raster for a filter layer over a \p viewportWidth by \p viewportHeight
/// surface. Primitives that read input away from the output position need the filtered content
/// just past the viewport, so the raster extends by the graph's input margin wherever the filter
/// region extends past the viewport, up to the whole region for a graph with unbounded reach. Each
/// axis stays within the surface dimension limit; a layout over the pixel limit falls back to the
/// viewport. Renderers also fall back to the viewport when the larger raster would not fit the
/// frame's filter budgets.
inline FilterInputBuffer ComputeFilterInputBuffer(const components::FilterGraph& graph,
                                                  const std::optional<Box2d>& filterRegion,
                                                  const Transform2d& deviceFromFilter,
                                                  int viewportWidth, int viewportHeight) {
  const FilterInputBuffer viewport{viewportWidth, viewportHeight, 0, 0};
  if (!filterRegion.has_value()) {
    return viewport;
  }
  const FilterInputMargin margin = ComputeFilterInputMargin(graph, deviceFromFilter);
  const double limit = static_cast<double>(components::kMaximumFilterSurfaceDimension);
  const Vector2d reach = margin.wholeRegion ? Vector2d(limit, limit) : margin.pixels;
  const Box2d region = deviceFromFilter.transformBox(*filterRegion);
  std::optional<Box2d> capture;
  if (margin.userCapture) {
    const Box2d source = deviceFromFilter.transformBox(*margin.userCapture);
    capture = Box2d(source.topLeft - margin.captureReach, source.bottomRight + margin.captureReach);
  }
  const auto captureAxis = [&](int axis) -> std::optional<std::pair<double, double>> {
    if (!capture) {
      return std::nullopt;
    }
    return axis == 0 ? std::pair(capture->topLeft.x, capture->bottomRight.x)
                     : std::pair(capture->topLeft.y, capture->bottomRight.y);
  };
  const FilterInputAxis x = LayoutFilterInputAxis({region.topLeft.x, region.bottomRight.x}, reach.x,
                                                  captureAxis(0), viewportWidth);
  const FilterInputAxis y = LayoutFilterInputAxis({region.topLeft.y, region.bottomRight.y}, reach.y,
                                                  captureAxis(1), viewportHeight);
  const FilterInputBuffer result{x.extent, y.extent, x.before, y.before};
  const std::uint64_t pixels = static_cast<std::uint64_t>(std::max(result.width, 0)) *
                               static_cast<std::uint64_t>(std::max(result.height, 0));
  return pixels > components::kMaximumFilterSurfacePixels ? viewport : result;
}

/// Invert only finite, nonsingular transforms whose inverse remains finite.
/// @param deviceFromFilter Transform to validate.
inline std::optional<Transform2d> FiniteFilterInverse(const Transform2d& deviceFromFilter) {
  const auto finite = [](const Transform2d& destinationFromSource) {
    return std::all_of(std::begin(destinationFromSource.data), std::end(destinationFromSource.data),
                       [](double value) { return std::isfinite(value); });
  };
  const double determinant = deviceFromFilter.determinant();
  if (!finite(deviceFromFilter) || !std::isfinite(determinant) || NearZero(determinant, 1e-12)) {
    return std::nullopt;
  }
  const Transform2d filterFromDevice = deviceFromFilter.inverse();
  return finite(filterFromDevice) ? std::optional(filterFromDevice) : std::nullopt;
}

/// Resolved clipping bounds and optional un-clipped animated geometry, in user coordinates.
struct FilterRegionSample {
  Box2d bounds;                         ///< Clip region, intersected with the filter region.
  std::optional<Box2d> animatedRegion;  ///< Un-clipped region for image placement when sampled.
};

/** Per-invocation filter coordinates and subregion history, independent of GPU resources.
 * Construction snapshots coordinate metadata. The caller records each completed primitive's
 * subregion in execution order; no document handles or borrowed transforms are retained.
 */
struct FilterRegionContext {
  using FilterGraph = components::FilterGraph;  ///< Filter graph type.
  using FilterInput = components::FilterInput;  ///< Primitive input reference type.
  using FilterNode = components::FilterNode;    ///< Primitive node type.

  /// Build a coordinate snapshot for the actual source raster and referencing filter.
  /// @param graph Effective filter units and object bounds.
  /// @param sourceSize Source raster dimensions in pixels.
  /// @param filterRegion Filter region in user coordinates.
  /// @param deviceFromFilter Filter-to-device transform, copied into this context.
  /// @param primitiveBounds Optional backend-resolved percentage reference bounds.
  /// @param rasterRegion Raster-space clipping region when default history follows raster
  /// execution.
  FilterRegionContext(const FilterGraph& graph, Vector2i sourceSize, const Box2d& filterRegion,
                      const Transform2d& deviceFromFilter,
                      std::optional<Box2d> primitiveBounds = std::nullopt,
                      std::optional<Box2d> rasterRegion = std::nullopt)
      : filterRegion(filterRegion),
        deviceFromFilter(deviceFromFilter),
        scaleX(FilterDeviceScaleX(deviceFromFilter)),
        scaleY(FilterDeviceScaleY(deviceFromFilter, scaleX)),
        isObjectBoundingBox(graph.primitiveUnits == PrimitiveUnits::ObjectBoundingBox),
        boundingBoxWidth(graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->width()
                                                              : 1.0),
        boundingBoxHeight(graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->height()
                                                               : 1.0),
        boundingBoxX(graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->topLeft.x
                                                          : 0.0),
        boundingBoxY(graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->topLeft.y
                                                          : 0.0),
        primitiveUnitsBounds(primitiveBounds.value_or(
            computePrimitiveUnitsBounds(sourceSize, graph.primitiveViewport))),
        historyInDeviceSpace(rasterRegion.has_value()),
        historyFilterRegion(rasterRegion.value_or(filterRegion)),
        historySourceRegion(rasterRegion ? Box2d::FromXYWH(0, 0, sourceSize.x, sourceSize.y)
                                         : filterRegion),
        previousOutputSubregion(historySourceRegion) {
    const auto maybeFilterFromDevice = FiniteFilterInverse(deviceFromFilter);
    invertible = maybeFilterFromDevice.has_value();
    filterFromDevice = maybeFilterFromDevice.value_or(Transform2d());
  }

  /// Resolve an effect length along X. @param value Length in primitive units.
  double toPixelX(double value) const {
    return FilterLengthInPixels(value, boundingBoxWidth, scaleX, isObjectBoundingBox);
  }

  /// Resolve an effect length along Y. @param value Length in primitive units.
  double toPixelY(double value) const {
    return FilterLengthInPixels(value, boundingBoxHeight, scaleY, isObjectBoundingBox);
  }

  /// Resolve an offset vector. @param dx X offset. @param dy Y offset.
  Vector2d toPixelOffset(double dx, double dy) const {
    return FilterOffsetInPixels({dx, dy}, {boundingBoxWidth, boundingBoxHeight},
                                isObjectBoundingBox, deviceFromFilter);
  }

  /// Look up an input region, falling back to the previous output for missing names.
  /// @param input Input reference.
  Box2d resolveInputSubregion(const FilterInput& input) const {
    if (const auto* named = std::get_if<FilterInput::Named>(&input.value)) {
      auto it = namedSubregions.find(named->name.str());
      return it != namedSubregions.end() ? it->second : previousOutputSubregion;
    }
    if (const auto* standard = std::get_if<components::FilterStandardInput>(&input.value)) {
      if (*standard == components::FilterStandardInput::FillPaint) {
        return fillPaintRegion.value_or(historySourceRegion);
      }
      if (*standard == components::FilterStandardInput::StrokePaint) {
        return strokePaintRegion.value_or(historySourceRegion);
      }
      return historySourceRegion;
    }
    return previousOutputSubregion;
  }

  /// Resolve explicit or input-derived primitive bounds. @param node Primitive to evaluate.
  Box2d computeNodeSubregion(const FilterNode& node) const {
    if (!invertible) {
      return Box2d::CreateEmpty(Vector2d::Zero());
    }
    if (node.hasExplicitRegion()) {
      return explicitNodeSubregion(node);
    }
    const Box2d bounds = computeDefaultSubregion(node);
    if (historyInDeviceSpace && (bounds.width() <= 0 || bounds.height() <= 0)) {
      return Box2d::CreateEmpty(filterFromDevice.transformPosition(bounds.topLeft));
    }
    return historyInDeviceSpace ? filterFromDevice.transformBox(bounds) : bounds;
  }

  /// Evaluate admitted region stacks after the default subregion is known.
  /// @param node Primitive with immutable recipes. Its execution cost must already be admitted.
  FilterRegionSample sampleNodeSubregion(const FilterNode& node) const {
    if (!invertible) {
      return {Box2d::CreateEmpty(Vector2d::Zero()), std::nullopt};
    }
    const Box2d base =
        node.hasExplicitRegion() ? explicitNodeRegion(node) : computeNodeSubregion(node);
    if (!node.hasAnimatedRegion()) {
      return {computeNodeSubregion(node), std::nullopt};
    }
    const std::array<std::optional<Lengthd>, 4> authored = {node.x, node.y, node.width,
                                                            node.height};
    std::array<double, 4> values = {base.topLeft.x, base.topLeft.y, base.width(), base.height()};
    bool sampled = false;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (const auto result = sampleRegionField(
              node, i, authored[i].value_or(Lengthd(values[i], Lengthd::Unit::Px)))) {
        values[i] = *result;
        sampled = true;
      }
    }
    const Box2d region = Box2d::FromXYWH(values[0], values[1], values[2], values[3]);
    return {intersect(region, filterRegion), sampled ? std::optional(region) : std::nullopt};
  }

  /// Retain a user-space primitive region for later input-derived defaults.
  /// @param node Completed primitive. @param bounds Region used for that primitive.
  void recordSubregion(const FilterNode& node, const Box2d& bounds) {
    storeSubregion(node, bounds);
  }

  /// Retain the raster executor's already-converted and bounded primitive region.
  /// @param node Completed primitive. @param bounds Explicit region, or absent for input defaults.
  void recordRasterSubregion(const FilterNode& node, std::optional<Box2d> bounds) {
    storeSubregion(
        node, bounds ? intersect(*bounds, historyFilterRegion) : computeDefaultSubregion(node));
  }

  /// Use admitted CPU paint-input coverage for standard-input default regions.
  /// @param fill FillPaint alpha bounds. @param stroke StrokePaint alpha bounds.
  void setPaintInputRegions(std::optional<Box2d> fill, std::optional<Box2d> stroke) {
    fillPaintRegion = fill;
    strokePaintRegion = stroke;
  }

  /// Resolve a position. @param length Value. @param extent Axis. @param origin Object origin.
  /// @param boundingBoxDimension Object extent on that axis.
  double resolvePrimitivePosition(const Lengthd& length, Lengthd::Extent extent, double origin,
                                  double boundingBoxDimension) const {
    return resolvePosition(length, extent, origin, boundingBoxDimension);
  }

  /// Resolve a size. @param length Value. @param extent Axis. @param boundingBoxDimension Extent.
  double resolvePrimitiveSize(const Lengthd& length, Lengthd::Extent extent,
                              double boundingBoxDimension) const {
    return resolveSize(length, extent, boundingBoxDimension);
  }

  Box2d filterRegion;                 ///< Filter region in user coordinates.
  Transform2d deviceFromFilter;       ///< Filter-to-device transform.
  double scaleX = 1.0;                ///< Device scale along X derived from deviceFromFilter.
  double scaleY = 1.0;                ///< Device scale along Y derived from deviceFromFilter.
  bool isObjectBoundingBox = false;   ///< Whether primitiveUnits is objectBoundingBox.
  double boundingBoxWidth = 1.0;      ///< Element bounding box width, or 1 without one.
  double boundingBoxHeight = 1.0;     ///< Element bounding box height, or 1 without one.
  double boundingBoxX = 0.0;          ///< Element bounding box left edge, or 0 without one.
  double boundingBoxY = 0.0;          ///< Element bounding box top edge, or 0 without one.
  bool invertible = true;             ///< Whether deviceFromFilter has a finite inverse.
  Transform2d filterFromDevice;       ///< Finite inverse of deviceFromFilter, or identity if none.
  Box2d primitiveUnitsBounds;         ///< Reference bounds for percentage primitive lengths.
  bool historyInDeviceSpace = false;  ///< Whether subregion history is kept in raster space.
  Box2d historyFilterRegion;          ///< Region that clips default subregions, in history space.
  Box2d historySourceRegion;          ///< Subregion of the standard inputs, in history space.
  std::optional<Box2d> fillPaintRegion;    ///< FillPaint coverage bounds, when admitted.
  std::optional<Box2d> strokePaintRegion;  ///< StrokePaint coverage bounds, when admitted.
  Box2d previousOutputSubregion;           ///< Subregion of the most recently recorded primitive.
  std::unordered_map<std::string, Box2d>
      namedSubregions;  ///< Subregions of recorded primitives, by result name.

private:
  void storeSubregion(const FilterNode& node, const Box2d& bounds) {
    previousOutputSubregion = bounds;
    if (node.result) {
      namedSubregions[node.result->str()] = bounds;
    }
  }

  Box2d computeDefaultSubregion(const FilterNode& node) const {
    using namespace components;
    const bool sourceGenerator =
        std::holds_alternative<filter_primitive::Flood>(node.primitive) ||
        std::holds_alternative<filter_primitive::Turbulence>(node.primitive) ||
        std::holds_alternative<filter_primitive::Image>(node.primitive) ||
        std::holds_alternative<filter_primitive::Tile>(node.primitive);
    const size_t slots = FilterInputSlotCount(node);
    if (sourceGenerator || slots == 0) {
      return historyFilterRegion;
    }
    Box2d inputBounds = resolveInputSubregion(FilterInputOrDefault(node, 0));
    for (size_t index = 1; index < slots; ++index) {
      inputBounds =
          Box2d::Union(inputBounds, resolveInputSubregion(FilterInputOrDefault(node, index)));
    }
    expandPrimitiveBounds(node, inputBounds);
    return intersect(inputBounds, historyFilterRegion);
  }

  double contextOrigin(double origin) const { return isObjectBoundingBox ? origin : 0; }

  std::optional<double> sampleRegionField(const FilterNode& node, std::size_t index,
                                          Lengthd underlying) const {
    const auto& recipe = node.animatedRegion[index];
    if (!recipe) {
      return std::nullopt;
    }
    components::AnimationValueContext context;
    context.viewBox = primitiveUnitsBounds;
    context.extent = index % 2 == 0 ? Lengthd::Extent::X : Lengthd::Extent::Y;
    const double dimension = index % 2 == 0 ? boundingBoxWidth : boundingBoxHeight;
    const double origin = index % 2 == 0 ? boundingBoxX : boundingBoxY;
    context.mappedLengths = true;
    context.unitlessScale = isObjectBoundingBox ? dimension : 1;
    context.unitlessOrigin = index < 2 ? contextOrigin(origin) : 0;
    context.percentageOrigin = context.unitlessOrigin;
    const auto sampled = components::SampleContextualLengthRecipe(*recipe, underlying, context);
    if (!sampled) {
      return std::nullopt;
    }
    const double value = index < 2 ? resolvePosition(*sampled, context.extent,
                                                     isObjectBoundingBox ? origin : 0, dimension)
                                   : resolveSize(*sampled, context.extent, dimension);
    return std::isfinite(value) ? std::optional(value) : std::nullopt;
  }

  Box2d computePrimitiveUnitsBounds(Vector2i sourceSize,
                                    const std::optional<Box2d>& viewport) const {
    if (isObjectBoundingBox) {
      return Box2d::FromXYWH(boundingBoxX, boundingBoxY, boundingBoxWidth, boundingBoxHeight);
    }
    if (viewport) {
      return *viewport;
    }
    const double userWidth = NearZero(scaleX, 1e-12) ? static_cast<double>(sourceSize.x)
                                                     : static_cast<double>(sourceSize.x) / scaleX;
    const double userHeight = NearZero(scaleY, 1e-12) ? static_cast<double>(sourceSize.y)
                                                      : static_cast<double>(sourceSize.y) / scaleY;
    return Box2d::FromXYWH(0.0, 0.0, userWidth, userHeight);
  }

  double resolvePosition(const Lengthd& length, Lengthd::Extent extent, double origin,
                         double boundingBoxDimension) const {
    if (isObjectBoundingBox && length.unit == Lengthd::Unit::None) {
      return origin + length.value * boundingBoxDimension;
    }
    if (length.unit == Lengthd::Unit::Percent) {
      const double referenceOrigin =
          isObjectBoundingBox ? (extent == Lengthd::Extent::X ? boundingBoxX : boundingBoxY) : 0.0;
      const double referenceSize =
          isObjectBoundingBox ? boundingBoxDimension
                              : (extent == Lengthd::Extent::X ? primitiveUnitsBounds.width()
                                                              : primitiveUnitsBounds.height());
      return referenceOrigin + referenceSize * length.value / 100.0;
    }
    return length.toPixels(primitiveUnitsBounds, FontMetrics(), extent);
  }

  double resolveSize(const Lengthd& length, Lengthd::Extent extent,
                     double boundingBoxDimension) const {
    if (isObjectBoundingBox && length.unit == Lengthd::Unit::None) {
      return length.value * boundingBoxDimension;
    }
    if (length.unit == Lengthd::Unit::Percent) {
      const double referenceSize =
          isObjectBoundingBox ? boundingBoxDimension
                              : (extent == Lengthd::Extent::X ? primitiveUnitsBounds.width()
                                                              : primitiveUnitsBounds.height());
      return referenceSize * length.value / 100.0;
    }
    return length.toPixels(primitiveUnitsBounds, FontMetrics(), extent);
  }

  static Box2d intersect(const Box2d& first, const Box2d& second) {
    const Vector2d minimum(std::max(first.topLeft.x, second.topLeft.x),
                           std::max(first.topLeft.y, second.topLeft.y));
    const Vector2d maximum(
        std::max(minimum.x, std::min(first.bottomRight.x, second.bottomRight.x)),
        std::max(minimum.y, std::min(first.bottomRight.y, second.bottomRight.y)));
    return Box2d(minimum, maximum);
  }

  Box2d explicitNodeSubregion(const FilterNode& node) const {
    return intersect(explicitNodeRegion(node), filterRegion);
  }

  Box2d explicitNodeRegion(const FilterNode& node) const {
    const double x = node.x.has_value() ? resolvePosition(*node.x, Lengthd::Extent::X,
                                                          isObjectBoundingBox ? boundingBoxX : 0.0,
                                                          boundingBoxWidth)
                                        : filterRegion.topLeft.x;
    const double y = node.y.has_value() ? resolvePosition(*node.y, Lengthd::Extent::Y,
                                                          isObjectBoundingBox ? boundingBoxY : 0.0,
                                                          boundingBoxHeight)
                                        : filterRegion.topLeft.y;
    const double width = node.width.has_value()
                             ? resolveSize(*node.width, Lengthd::Extent::X, boundingBoxWidth)
                             : filterRegion.width();
    const double height = node.height.has_value()
                              ? resolveSize(*node.height, Lengthd::Extent::Y, boundingBoxHeight)
                              : filterRegion.height();
    return Box2d(Vector2d(x, y), Vector2d(x + width, y + height));
  }

  Box2d expandByDeviceSupport(const Box2d& bounds, double x, double y) const {
    if (historyInDeviceSpace) {
      return Box2d(bounds.topLeft - Vector2d(x, y), bounds.bottomRight + Vector2d(x, y));
    }
    const Vector2d extent(
        std::abs(filterFromDevice.data[0]) * x + std::abs(filterFromDevice.data[2]) * y,
        std::abs(filterFromDevice.data[1]) * x + std::abs(filterFromDevice.data[3]) * y);
    return Box2d(bounds.topLeft - extent, bounds.bottomRight + extent);
  }

  void expandPrimitiveBounds(const FilterNode& node, Box2d& inputBounds) const {
    using namespace components;
    if (const auto* blur = std::get_if<filter_primitive::GaussianBlur>(&node.primitive)) {
      const double x = std::ceil((blur->stdDeviationX >= 0
                                      ? BoundedPositiveFilterPixels(toPixelX(blur->stdDeviationX))
                                      : 0.0) *
                                 3.0);
      const double y = std::ceil((blur->stdDeviationY >= 0
                                      ? BoundedPositiveFilterPixels(toPixelY(blur->stdDeviationY))
                                      : 0.0) *
                                 3.0);
      inputBounds = expandByDeviceSupport(inputBounds, x, y);
      return;
    }
    if (const auto* drop = std::get_if<filter_primitive::DropShadow>(&node.primitive)) {
      expandDropShadow(*drop, inputBounds);
      return;
    }
    if (const auto* morphology = std::get_if<filter_primitive::Morphology>(&node.primitive);
        morphology && morphology->op == filter_primitive::Morphology::Operator::Dilate) {
      if (morphology->radiusX < 0 || morphology->radiusY < 0) {
        return;
      }
      const double x = std::round(BoundedPositiveFilterPixels(toPixelX(morphology->radiusX)));
      const double y = std::round(BoundedPositiveFilterPixels(toPixelY(morphology->radiusY)));
      inputBounds = expandByDeviceSupport(inputBounds, x, y);
    }
  }

  void expandDropShadow(const components::filter_primitive::DropShadow& drop,
                        Box2d& inputBounds) const {
    const double x = std::ceil((drop.stdDeviationX >= 0
                                    ? BoundedPositiveFilterPixels(toPixelX(drop.stdDeviationX))
                                    : 0.0) *
                               3.0);
    const double y = std::ceil((drop.stdDeviationY >= 0
                                    ? BoundedPositiveFilterPixels(toPixelY(drop.stdDeviationY))
                                    : 0.0) *
                               3.0);
    const Vector2d offset = toPixelOffset(drop.dx, drop.dy);
    const Vector2d rasterOffset(std::round(BoundedSignedFilterPixels(offset.x)),
                                std::round(BoundedSignedFilterPixels(offset.y)));
    const Vector2d boundedOffset =
        historyInDeviceSpace ? rasterOffset : filterFromDevice.transformVector(rasterOffset);
    const Box2d expanded = expandByDeviceSupport(inputBounds, x, y);
    const Box2d shadow(expanded.topLeft + boundedOffset, expanded.bottomRight + boundedOffset);
    inputBounds = Box2d::Union(inputBounds, shadow);
  }
};

}  // namespace donner::svg
