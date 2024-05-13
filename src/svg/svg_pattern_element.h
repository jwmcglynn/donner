#pragma once
/// @file

#include <optional>

#include "src/base/length.h"
#include "src/base/transform.h"
#include "src/svg/core/pattern.h"
#include "src/svg/core/preserve_aspect_ratio.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

/**
 * @defgroup xml_pattern '<pattern>'
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
 * \todo: Add documentation for parameters.
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
 */
class SVGPatternElement : public SVGElement {
protected:
  explicit SVGPatternElement(EntityHandle handle) : SVGElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Pattern;
  static constexpr std::string_view Tag{"pattern"};

  static SVGPatternElement Create(SVGDocument& document);

  std::optional<Boxd> viewbox() const;
  PreserveAspectRatio preserveAspectRatio() const;
  Lengthd x() const;
  Lengthd y() const;
  std::optional<Lengthd> width() const;
  std::optional<Lengthd> height() const;
  PatternUnits patternUnits() const;
  PatternContentUnits patternContentUnits() const;
  Transformd patternTransform() const;
  std::optional<RcString> href() const;

  void setViewbox(std::optional<Boxd> viewbox);
  void setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio);
  void setX(Lengthd value);
  void setY(Lengthd value);
  void setWidth(std::optional<Lengthd> value);
  void setHeight(std::optional<Lengthd> value);
  void setPatternUnits(PatternUnits value);
  void setPatternContentUnits(PatternContentUnits value);
  void setPatternTransform(Transformd transform);
  void setHref(const std::optional<RcString>& value);

protected:
  void invalidateTransform();
  void computeTransform() const;
};

}  // namespace donner::svg
