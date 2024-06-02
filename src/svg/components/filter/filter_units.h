#pragma once
/// @file

namespace donner::svg {

/**
 * Values for the `"filterUnits"` attribute which defines the coordinate system for attributes `x`,
 * `y`, `width`, `height`.
 *
 * @see https://www.w3.org/TR/filter-effects/#element-attrdef-filter-filterunits.
 *
 * This is used on \ref xml_filter element.
 */
enum class FilterUnits {
  /**
   * If filterUnits is equal to userSpaceOnUse, `x`, `y`, `width`, `height` represent values in the
   * current user coordinate system in place at the time when the filter element is referenced
   * (i.e., the user coordinate system for the element referencing the filter element via a filter
   * property).
   */
  UserSpaceOnUse,
  /**
   * If filterUnits is equal to objectBoundingBox, then x, y, width, height represent fractions or
   * percentages of the bounding box on the referencing element (see object bounding box units).
   */
  ObjectBoundingBox,
  /**
   * The default value for the `"filterUnits"` attribute, which is `objectBoundingBox`.
   */
  Default = ObjectBoundingBox,
};

/**
 * Values for the `"primitiveUnits"` attribute which specifies the coordinate system for the various
 * length values within the filter primitives and for the attributes that define the filter
 * primitive subregion.
 *
 * @see https://www.w3.org/TR/filter-effects/#element-attrdef-filter-primitiveunits
 *
 * This is used on \ref xml_filter element.
 */
enum class PrimitiveUnits {
  /**
   * If primitiveUnits is equal to userSpaceOnUse, any length values within the filter definitions
   * represent values in the current local coordinate system in place at the time when the \ref
   * xml_filter element is referenced (i.e., the user coordinate system for the element referencing
   * the filter element via a filter property).
   */
  UserSpaceOnUse,
  /**
   * If primitiveUnits is equal to objectBoundingBox, then any length values within the filter
   * definitions represent fractions or percentages of the bounding box on the referencing element
   * (see [object bounding box
   * units](https://svgwg.org/svg2-draft/coords.html#ObjectBoundingBoxUnits)). Note that if only one
   * number was specified in a `<number-optional-number>` value this number is expanded out before
   * the primitiveUnits computation takes place.
   */
  ObjectBoundingBox,
  /**
   * The default value for the `"primitiveUnits"` attribute, which is `userSpaceOnUse`.
   */
  Default = UserSpaceOnUse,
};

}  // namespace donner::svg
