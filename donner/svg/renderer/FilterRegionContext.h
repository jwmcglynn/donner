#pragma once
/// @file

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <variant>

#include "donner/base/MathUtils.h"
#include "donner/base/Transform.h"
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

/** Per-invocation filter coordinates and subregion history, independent of GPU resources.
 * Construction snapshots coordinate metadata. The caller records each completed primitive's
 * subregion in execution order; no document handles or borrowed transforms are retained.
 */
struct FilterRegionContext {
  using FilterGraph = components::FilterGraph;
  using FilterInput = components::FilterInput;
  using FilterNode = components::FilterNode;

  /// Build a coordinate snapshot for the actual source raster and referencing filter.
  /// @param graph Effective filter units and object bounds.
  /// @param sourceSize Source raster dimensions in pixels.
  /// @param filterRegion Filter region in user coordinates.
  /// @param deviceFromFilter Filter-to-device transform, copied into this context.
  FilterRegionContext(const FilterGraph& graph, Vector2i sourceSize, const Box2d& filterRegion,
                      const Transform2d& deviceFromFilter)
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
        filterFromDevice(deviceFromFilter.inverse()),
        primitiveUnitsBounds(computePrimitiveUnitsBounds(sourceSize)),
        previousOutputSubregion(filterRegion) {}

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
    return std::holds_alternative<FilterInput::Previous>(input.value) ? previousOutputSubregion
                                                                      : filterRegion;
  }

  /// Resolve explicit or input-derived primitive bounds. @param node Primitive to evaluate.
  Box2d computeNodeSubregion(const FilterNode& node) const {
    using namespace components;
    const bool sourceGenerator =
        std::holds_alternative<filter_primitive::Flood>(node.primitive) ||
        std::holds_alternative<filter_primitive::Turbulence>(node.primitive) ||
        std::holds_alternative<filter_primitive::Image>(node.primitive) ||
        std::holds_alternative<filter_primitive::Tile>(node.primitive);
    const bool explicitSubregion = node.x.has_value() || node.y.has_value() ||
                                   node.width.has_value() || node.height.has_value();
    if (explicitSubregion) {
      return explicitNodeSubregion(node);
    }
    const size_t slots = FilterInputSlotCount(node);
    if (sourceGenerator || slots == 0) {
      return filterRegion;
    }
    Box2d inputBounds = resolveInputSubregion(FilterInputOrDefault(node, 0));
    for (size_t index = 1; index < slots; ++index) {
      inputBounds =
          Box2d::Union(inputBounds, resolveInputSubregion(FilterInputOrDefault(node, index)));
    }
    expandPrimitiveBounds(node, inputBounds);
    return intersect(inputBounds, filterRegion);
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

  Box2d filterRegion;
  Transform2d deviceFromFilter;
  double scaleX = 1.0;
  double scaleY = 1.0;
  bool isObjectBoundingBox = false;
  double boundingBoxWidth = 1.0;
  double boundingBoxHeight = 1.0;
  double boundingBoxX = 0.0;
  double boundingBoxY = 0.0;
  Transform2d filterFromDevice;
  Box2d primitiveUnitsBounds;
  Box2d previousOutputSubregion;
  std::unordered_map<std::string, Box2d> namedSubregions;

private:
  Box2d computePrimitiveUnitsBounds(Vector2i sourceSize) const {
    if (isObjectBoundingBox) {
      return Box2d::FromXYWH(boundingBoxX, boundingBoxY, boundingBoxWidth, boundingBoxHeight);
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
    return Box2d(Vector2d(std::max(first.topLeft.x, second.topLeft.x),
                          std::max(first.topLeft.y, second.topLeft.y)),
                 Vector2d(std::min(first.bottomRight.x, second.bottomRight.x),
                          std::min(first.bottomRight.y, second.bottomRight.y)));
  }

  Box2d explicitNodeSubregion(const FilterNode& node) const {
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
    return intersect(Box2d(Vector2d(x, y), Vector2d(x + width, y + height)), filterRegion);
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
      inputBounds = Box2d(Vector2d(inputBounds.topLeft.x - x, inputBounds.topLeft.y - y),
                          Vector2d(inputBounds.bottomRight.x + x, inputBounds.bottomRight.y + y));
      return;
    }
    if (const auto* drop = std::get_if<filter_primitive::DropShadow>(&node.primitive)) {
      expandDropShadow(*drop, inputBounds);
      return;
    }
    if (const auto* morphology = std::get_if<filter_primitive::Morphology>(&node.primitive);
        morphology && morphology->op == filter_primitive::Morphology::Operator::Dilate) {
      const double x = BoundedPositiveFilterPixels(toPixelX(morphology->radiusX));
      const double y = BoundedPositiveFilterPixels(toPixelY(morphology->radiusY));
      inputBounds = Box2d(Vector2d(inputBounds.topLeft.x - x, inputBounds.topLeft.y - y),
                          Vector2d(inputBounds.bottomRight.x + x, inputBounds.bottomRight.y + y));
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
    const Vector2d boundedOffset(BoundedSignedFilterPixels(offset.x),
                                 BoundedSignedFilterPixels(offset.y));
    const Box2d shadow(Vector2d(inputBounds.topLeft.x + boundedOffset.x - x,
                                inputBounds.topLeft.y + boundedOffset.y - y),
                       Vector2d(inputBounds.bottomRight.x + boundedOffset.x + x,
                                inputBounds.bottomRight.y + boundedOffset.y + y));
    inputBounds = Box2d::Union(inputBounds, shadow);
  }
};

}  // namespace donner::svg
