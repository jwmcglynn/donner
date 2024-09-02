#pragma once
/// @file

#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/core/Pattern.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

/**
 * Parameters for \ref xml_pattern elements which are not captured by \ref ViewboxComponent and \ref
 * SizedElementComponent.
 */
struct PatternComponent {
  /// The pattern units of the pattern, if \c std::nullopt, the pattern units are inherited from the
  /// parent or defaulted to \ref PatternUnits::Default.
  std::optional<PatternUnits> patternUnits;
  /// The pattern content units of the pattern, if \c std::nullopt, the pattern content units are
  /// inherited from the parent or defaulted to \ref PatternContentUnits::Default.
  std::optional<PatternContentUnits> patternContentUnits;
  /// An optional href to another pattern, which is used to inherit properties from if not by this
  /// entity.
  std::optional<Reference> href;
  /// Contains the `x`, `y`, `width`, and `height` properties of the pattern tile rectangle.
  SizedElementProperties sizeProperties;
};

/**
 * Computed properties for \ref xml_pattern elements, after resolving and inheriting values from
 * referenced patterns.
 */
struct ComputedPatternComponent {
  /// True if this component has been initialized, false otherwise.
  bool initialized = false;

  /// The pattern units of the pattern, defaults to \ref PatternUnits::Default.
  PatternUnits patternUnits = PatternUnits::Default;
  /// The pattern content units of the pattern, defaults to \ref PatternContentUnits::Default.
  PatternContentUnits patternContentUnits = PatternContentUnits::Default;
  /// The pattern tile rectangle, defaults to the empty rect.
  Boxd tileRect = Boxd::CreateEmpty(Vector2d());
  /// The preserveAspectRatio of the pattern, defaults to \ref PreserveAspectRatio::None.
  PreserveAspectRatio preserveAspectRatio;
  /// The viewbox of the pattern, or \c std::nullopt if not set.
  std::optional<Boxd> viewbox;
  /// Resolved `x`, `y`, `width`, and `height` properties of the pattern tile rectangle.
  SizedElementProperties sizeProperties;

  /**
   * Inherit attributes from the referenced pattern.
   *
   * @param handle The handle of the pattern entity.
   * @param base The handle of the base entity to copy from.
   */
  void inheritAttributesFrom(EntityHandle handle, EntityHandle base);
};

}  // namespace donner::svg::components
