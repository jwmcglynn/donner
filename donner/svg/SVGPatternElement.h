#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/base/RcStringOrRef.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/core/Pattern.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg {

/**
 * @page xml_pattern "<pattern>"
 * @ingroup elements_paint_servers
 *
 * Defines a paint server containing a repeated pattern, which is tiled to fill the area.
 *
 * - DOM object: SVGPatternElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/pservers.html#Patterns
 *
 * These elements are typically placed within a \ref xml_defs element, and then referenced by id
 * from a `fill` or `stroke` attribute.
 *
 * ```xml
 * <pattern id="MyPattern" viewbox="0,0,10,10" width="15%" height="15%">
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
 *     <pattern id="MyPattern" viewbox="0,0,10,10" width="15%" height="15%">
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

/**
 * DOM object for a \ref xml_pattern element.
 *
 * ```xml
 * <pattern id="MyPattern" viewbox="0,0,10,10" width="15%" height="15%">
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
 *     <pattern id="MyPattern" viewbox="0,0,10,10" width="15%" height="15%">
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
protected:
  /// Create an SVGPatternElement wrapper from an entity.
  explicit SVGPatternElement(EntityHandle handle) : SVGElement(handle) {}

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
  static SVGPatternElement Create(SVGDocument& document);

  /**
   * Get the parsed value of the `viewBox` attribute, if specified, which defines a rectangle in
   * userspace that should be mapped to the SVG viewport bounds established by the pattern.
   */
  std::optional<Boxd> viewbox() const;

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
  Transformd patternTransform() const;

  /**
   * Get the value of the `href` attribute, if specified, which is a reference to another pattern
   * element to use as a template.
   */
  std::optional<RcString> href() const;

  /**
   * Set the `viewBox` attribute, which defines a rectangle in userspace that should be mapped to
   * the SVG viewport bounds established by the pattern.
   *
   * @param viewbox The viewBox value to set.
   */
  void setViewbox(std::optional<Boxd> viewbox);

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
  void setWidth(std::optional<Lengthd> value);

  /**
   * Set the `height` attribute, which defines the height of the rectangle indicating how pattern
   * tiles are placed and spread.
   *
   * @param value The height value to set.
   */
  void setHeight(std::optional<Lengthd> value);

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
  void setPatternTransform(Transformd transform);

  /**
   * Set the `href` attribute, which is a reference to another pattern element to use as a template.
   *
   * @param value The href value to set.
   */
  void setHref(const std::optional<RcStringOrRef>& value);
};

}  // namespace donner::svg
