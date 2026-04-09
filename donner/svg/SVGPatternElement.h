#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/base/OptionalRef.h"
#include "donner/base/RcStringOrRef.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/core/Pattern.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg {

/**
 * @page xml_pattern "<pattern>"
 *
 * Defines a paint server containing a repeated pattern, which is tiled to fill the area.
 *
 * - DOM object: SVGPatternElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/pservers.html#Patterns
 *
 * A `<pattern>` is a **tileable paint server**: you draw a small graphic once inside the
 * `<pattern>` element, then fill any shape with `fill="url(#id)"` and SVG repeats (tiles) that
 * graphic across the filled region. Unlike gradients, which vary color smoothly, a pattern
 * repeats an arbitrary sub-drawing — stripes, dots, crosshatches, custom textures — as many
 * times as needed to cover the shape.
 *
 * Patterns are typically declared inside a \ref xml_defs block. The `width` and `height`
 * control the size of each tile (in either object-bounding-box or user-space units depending
 * on `patternUnits`), and the `viewBox` lets the pattern contents author their own coordinate
 * system. For smooth color transitions, use \ref xml_linearGradient or \ref xml_radialGradient
 * instead.
 *
 * ```xml
 * <pattern id="MyPattern" viewBox="0,0,10,10" width="15%" height="15%">
 *   <circle cx="5" cy="5" r="5" fill="red" />
 * </pattern>
 * ```
 *
 * To reference it with a fill:
 * ```xml
 * <rect fill="url(#MyPattern)" width="300" height="300" />
 * ```
 *
 * \htmlonly
 * <svg id="xml_pattern_basic" width="300" height="300">
 *   <defs>
 *     <pattern id="xml_pattern_basic_dots" viewBox="0,0,10,10" width="15%" height="15%">
 *       <circle cx="5" cy="5" r="5" fill="red" />
 *     </pattern>
 *   </defs>
 *   <rect fill="url(#xml_pattern_basic_dots)" width="300" height="300" />
 * </svg>
 * \endhtmlonly
 *
 * # patternUnits: objectBoundingBox vs userSpaceOnUse
 *
 * The `patternUnits` attribute controls how the pattern's tile rectangle (`x`, `y`, `width`,
 * `height`) is interpreted. The two modes produce very different results when the same pattern
 * is applied to shapes of different sizes.
 *
 * - **`objectBoundingBox`** (default): the tile size is a **fraction of the filled shape's
 *   bounding box**. A 20% × 20% tile produces 5 × 5 tiles across any shape, so the pattern
 *   **scales with the shape**. Useful for "N tiles across" effects regardless of shape size.
 * - **`userSpaceOnUse`**: the tile size is in **absolute user-space units** (pixels). A 20 × 20
 *   pixel tile produces more or fewer tiles depending on how large the shape is, so the pattern
 *   **stays the same physical size** across shapes. Useful for repeating textures that
 *   shouldn't distort with scale.
 *
 * \htmlonly
 * <svg id="xml_pattern_units" width="400" height="160" viewBox="0 0 400 160" style="background-color: white" font-family="sans-serif" font-size="11">
 *   <defs>
 *     <pattern id="xml_pattern_units_obb" patternUnits="objectBoundingBox" width="20%" height="20%">
 *       <circle cx="5" cy="5" r="4" fill="#e0a63a" />
 *     </pattern>
 *     <pattern id="xml_pattern_units_uso" patternUnits="userSpaceOnUse" width="20" height="20">
 *       <circle cx="10" cy="10" r="4" fill="#4a90e2" />
 *     </pattern>
 *   </defs>
 *   <text x="115" y="15" text-anchor="middle" font-weight="bold">objectBoundingBox</text>
 *   <rect x="20"  y="25" width="60"  height="60"  fill="url(#xml_pattern_units_obb)" stroke="#555" />
 *   <rect x="90"  y="25" width="120" height="60"  fill="url(#xml_pattern_units_obb)" stroke="#555" />
 *   <text x="115" y="105" text-anchor="middle">5 × 5 tiles regardless of size</text>
 *   <text x="315" y="15"  text-anchor="middle" font-weight="bold">userSpaceOnUse</text>
 *   <rect x="220" y="25" width="60"  height="60"  fill="url(#xml_pattern_units_uso)" stroke="#555" />
 *   <rect x="290" y="25" width="120" height="60"  fill="url(#xml_pattern_units_uso)" stroke="#555" />
 *   <text x="315" y="105" text-anchor="middle">20px tiles; bigger shape = more tiles</text>
 *   <text x="200" y="140" text-anchor="middle" fill="#555">(same pattern definition, different patternUnits)</text>
 * </svg>
 * \endhtmlonly
 *
 * # patternTransform
 *
 * `patternTransform` applies an extra transformation to the tile grid — you can rotate, skew,
 * or scale the tiling without affecting the filled shape itself. Common uses include diagonal
 * stripes (rotate 45°) and textures drawn "on an angle".
 *
 * \htmlonly
 * <svg id="xml_pattern_transform" width="400" height="140" viewBox="0 0 400 140" style="background-color: white" font-family="sans-serif" font-size="11">
 *   <defs>
 *     <pattern id="xml_pattern_transform_s0" patternUnits="userSpaceOnUse" width="16" height="16">
 *       <rect width="16" height="16" fill="#f4e6c8" />
 *       <rect width="8" height="16" fill="#7a5a1a" />
 *     </pattern>
 *     <pattern id="xml_pattern_transform_s45" patternUnits="userSpaceOnUse" width="16" height="16" patternTransform="rotate(45)">
 *       <rect width="16" height="16" fill="#f4e6c8" />
 *       <rect width="8" height="16" fill="#7a5a1a" />
 *     </pattern>
 *     <pattern id="xml_pattern_transform_sScaled" patternUnits="userSpaceOnUse" width="16" height="16" patternTransform="rotate(45) scale(0.6)">
 *       <rect width="16" height="16" fill="#f4e6c8" />
 *       <rect width="8" height="16" fill="#7a5a1a" />
 *     </pattern>
 *   </defs>
 *   <rect x="20"  y="20" width="100" height="80" fill="url(#xml_pattern_transform_s0)" stroke="#555" />
 *   <text x="70" y="115" text-anchor="middle">no transform</text>
 *   <rect x="150" y="20" width="100" height="80" fill="url(#xml_pattern_transform_s45)" stroke="#555" />
 *   <text x="200" y="115" text-anchor="middle">rotate(45)</text>
 *   <rect x="280" y="20" width="100" height="80" fill="url(#xml_pattern_transform_sScaled)" stroke="#555" />
 *   <text x="330" y="115" text-anchor="middle">rotate(45) scale(0.6)</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `viewBox` | (none)  | A list of four numbers (min-x, min-y, width, height) that specifies a rectangle in userspace mapped to the pattern viewport. |
 * | `preserveAspectRatio` | `xMidYMid meet` | How to scale the viewport to fit the content. Only applies if `viewBox` is specified. |
 * | `x`       | `0`     | Top-left X of the tile rectangle. Interpretation depends on `patternUnits`. |
 * | `y`       | `0`     | Top-left Y of the tile rectangle. Interpretation depends on `patternUnits`. |
 * | `width`   | `0`     | Width of one tile. Interpretation depends on `patternUnits`. |
 * | `height`  | `0`     | Height of one tile. Interpretation depends on `patternUnits`. |
 * | `patternUnits` | `objectBoundingBox` | Coordinate system for `x`, `y`, `width`, `height`: `objectBoundingBox` (fraction of the filled shape's bounds) or `userSpaceOnUse` (absolute user-space units). |
 * | `patternContentUnits` | `userSpaceOnUse` | Coordinate system for the pattern contents. Has no effect when `viewBox` is specified. |
 * | `patternTransform` | identity | Additional transform applied to the tile grid (rotate, skew, scale) without affecting the filled shape. |
 * | `href`   | (none)  | Reference to another pattern element to use as a template. |
 */

/**
 * DOM object for a \ref xml_pattern element.
 *
 * ```xml
 * <pattern id="MyPattern" viewBox="0,0,10,10" width="15%" height="15%">
 *   <circle cx="5" cy="5" r="5" fill="red" />
 * </pattern>
 * ```
 *
 * To reference it with a fill:
 * ```xml
 * <rect fill="url(#MyPattern)" width="300" height="300" />
 * ```
 *
 * \htmlonly
 * <svg width="300" height="300">
 *   <defs>
 *     <pattern id="MyPattern" viewBox="0,0,10,10" width="15%" height="15%">
 *       <circle cx="5" cy="5" r="5" fill="red" />
 *     </pattern>
 *   </defs>
 *   <rect fill="url(#MyPattern)" width="300" height="300" />
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `viewBox` | (none)  | A list of four numbers (min-x, min-y, width, height) separated by whitespace and/or a comma, that specify a rectangle in userspace that should be mapped to the SVG viewport bounds established by the pattern. |
 * | `preserveAspectRatio` | `xMidYMid meet` | How to scale the viewport to fit the content. Only applies is `viewBox` is specified. |
 * | `x`       | `0`     | Defines the top-left X coordinate of a rectangle indicating how pattern tiles are placed and spread. The coordinate system is determined by the combination of the `patternUnits` and `patternTransform` attributes. |
 * | `y`       | `0`     | Defines the top-left Y coordinate of a rectangle indicating how pattern tiles are placed and spread. The coordinate system is determined by the combination of the `patternUnits` and `patternTransform` attributes. |
 * | `width`   | `0`     | Defines the width of a rectangle indicating how pattern tiles are placed and spread. The coordinate system is determined by the combination of the `patternUnits` and `patternTransform` attributes. |
 * | `height`  | `0`     | Defines the height of a rectangle indicating how pattern tiles are placed and spread. The coordinate system is determined by the combination of the `patternUnits` and `patternTransform` attributes. |
 * | `patternUnits` | `objectBoundingBox` | Defines the coordinate system for attributes `x`, `y`, `width`, and `height`. |
 * | `patternContentUnits` | `userSpaceOnUse` | Defines the coordinate system for the contents of the pattern. Note that this attribute has no effect if the `viewBox` attribute is specified. |
 * | `patternTransform` | identity | Optional transformation from the pattern coordinate system onto the target coordinate system, allowing things like skewing the pattern tiles. |
 * | `href`   | (none)  | Reference to another pattern element to use as a template. |
 */
class SVGPatternElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGPatternElement wrapper from an entity.
  explicit SVGPatternElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGPatternElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Pattern;
  /// XML tag name, \ref xml_pattern.
  static constexpr std::string_view Tag{"pattern"};

  /**
   * Create a new \ref xml_pattern element.
   *
   * @param document Containing document.
   */
  static SVGPatternElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

  /**
   * Get the parsed value of the `viewBox` attribute, if specified, which defines a rectangle in
   * userspace that should be mapped to the SVG viewport bounds established by the pattern.
   */
  std::optional<Box2d> viewBox() const;

  /**
   * The value of the `preserveAspectRatio` attribute, which defines how to scale the viewport to
   * fit the content. Only applies is `viewBox` is specified.
   */
  PreserveAspectRatio preserveAspectRatio() const;

  /**
   * Get the value of the `x` attribute, which defines the top-left X coordinate of the rectangle
   * indicating how pattern tiles are placed and spread.
   */
  Lengthd x() const;

  /**
   * Get the value of the `y` attribute, which defines the top-left Y coordinate of the rectangle
   * indicating how pattern tiles are placed and spread.
   */
  Lengthd y() const;

  /**
   * Get the value of the `width` attribute, if specified, which defines the width of the rectangle
   * indicating how pattern tiles are placed and spread.
   */
  std::optional<Lengthd> width() const;

  /**
   * Get the value of the `height` attribute, if specified, which defines the height of the
   * rectangle indicating how pattern tiles are placed and spread.
   */
  std::optional<Lengthd> height() const;

  /**
   * Get the value of the `patternUnits` attribute, which defines the coordinate system for
   * attributes `x`, `y`, `width`, and `height`.
   */
  PatternUnits patternUnits() const;

  /**
   * Get the value of the `patternContentUnits` attribute, which defines the coordinate system for
   * the contents of the pattern. Note that this attribute has no effect if the `viewBox` attribute
   * is specified.
   */
  PatternContentUnits patternContentUnits() const;

  /**
   * Get the value of the `patternTransform` attribute, which is an optional transformation from the
   * pattern coordinate system onto the target coordinate system, allowing transformations such as
   * skewing the pattern tiles.
   */
  Transform2d patternTransform() const;

  /**
   * Get the value of the `href` attribute, if specified, which is a reference to another pattern
   * element to use as a template.
   */
  std::optional<RcString> href() const;

  /**
   * Set the `viewBox` attribute, which defines a rectangle in userspace that should be mapped to
   * the SVG viewport bounds established by the pattern.
   *
   * @param viewBox The viewBox value to set.
   */
  void setViewBox(OptionalRef<Box2d> viewBox);

  /**
   * Set the `preserveAspectRatio` attribute, which defines how to scale the viewport to fit the
   * content. Only applies if `viewBox` is specified.
   *
   * @param preserveAspectRatio The preserveAspectRatio value to set.
   */
  void setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio);

  /**
   * Set the `x` attribute, which defines the top-left X coordinate of the rectangle indicating how
   * pattern tiles are placed and spread.
   *
   * @param value The X coordinate value to set.
   */
  void setX(Lengthd value);

  /**
   * Set the `y` attribute, which defines the top-left Y coordinate of the rectangle indicating how
   * pattern tiles are placed and spread.
   *
   * @param value The Y coordinate value to set.
   */
  void setY(Lengthd value);

  /**
   * Set the `width` attribute, which defines the width of the rectangle indicating how pattern
   * tiles are placed and spread.
   *
   * @param value The width value to set.
   */
  void setWidth(OptionalRef<Lengthd> value);

  /**
   * Set the `height` attribute, which defines the height of the rectangle indicating how pattern
   * tiles are placed and spread.
   *
   * @param value The height value to set.
   */
  void setHeight(OptionalRef<Lengthd> value);

  /**
   * Set the `patternUnits` attribute, which defines the coordinate system for attributes `x`, `y`,
   * `width`, and `height`.
   *
   * @param value The patternUnits value to set.
   */
  void setPatternUnits(PatternUnits value);

  /**
   * Set the `patternContentUnits` attribute, which defines the coordinate system for the contents
   * of the pattern.
   *
   * @param value The patternContentUnits value to set.
   */
  void setPatternContentUnits(PatternContentUnits value);

  /**
   * Set the `patternTransform` attribute, which is an optional transformation from the pattern
   * coordinate system onto the target coordinate system.
   *
   * @param transform The patternTransform value to set.
   */
  void setPatternTransform(Transform2d transform);

  /**
   * Set the `href` attribute, which is a reference to another pattern element to use as a template.
   *
   * @param value The href value to set.
   */
  void setHref(OptionalRef<RcStringOrRef> value);
};

}  // namespace donner::svg
