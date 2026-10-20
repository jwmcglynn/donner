#pragma once
/// @file

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
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
