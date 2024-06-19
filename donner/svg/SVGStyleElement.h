#pragma once
/// @file

#include "donner/base/RcStringOrRef.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {

// clang-format off
/**
 * @defgroup xml_style '<style>'
 *
 * Defines a CSS stylesheet for the document. Multiple \ref xml_style elements may be defined in a
 * single document, and the aggregate document style is computed from that using CSS cascading
 * rules.
 *
 * Stylesheets support CSS Level 3: https://www.w3.org/TR/css-syntax-3/
 *
 * - DOM object: SVGStyleElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/styling.html#StyleElement
 *
 * ```xml
 * <style>
 *   .myRect { fill: red; stroke: black; stroke-width: 2px; }
 *   circle { fill: black }
 *   circle:nth-child(2n) { fill: green }
 * </style>
 * ```
 *
 * \htmlonly
 * <svg id="xml_style" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_style .myRect { fill: red; stroke: black; stroke-width: 2px; }
 *     #xml_style circle { fill: black }
 *     #xml_style circle:nth-child(2n) { fill: green }
 *   </style>
 *   <rect id="myRect" x="0" y="0" width="100" height="25" />
 *   <circle cx="50" cy="50" r="20"/>
 *   <circle cx="100" cy="50" r="20"/>
 *   <circle cx="150" cy="50" r="20"/>
 *   <circle cx="200" cy="50" r="20"/>
 *   <circle cx="250" cy="50" r="20"/>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default    | Description  |
 * | --------: | :--------: | :----------- |
 * | `type`    | `text/css` | Type of the stylesheets contents, currently only `text/css` is supported. |
 * | `media`   | (empty)    | [unsupported] Specifies a media query that must be matched for the style sheet to apply. |
 * | `title`   | (empty)    | [unsupported] Specifies a title for the style sheet, which is used when selecting between alternate style sheets. |
 *
 * Note that `type` is defined to be a media type [[rfc2046](https://www.ietf.org/rfc/rfc2046.txt)].
 */

/**
 * DOM object for a \ref xml_style element, which contains a CSS stylesheet.
 *
 * \htmlonly
 * <svg id="xml_style" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_style .myRect { fill: red; stroke: black; stroke-width: 2px; }
 *     #xml_style circle { fill: black }
 *     #xml_style circle:nth-child(2n) { fill: green }
 *   </style>
 *   <rect id="myRect" x="0" y="0" width="100" height="25" />
 *   <circle cx="50" cy="50" r="20"/>
 *   <circle cx="100" cy="50" r="20"/>
 *   <circle cx="150" cy="50" r="20"/>
 *   <circle cx="200" cy="50" r="20"/>
 *   <circle cx="250" cy="50" r="20"/>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default    | Description  |
 * | --------: | :--------: | :----------- |
 * | `type`    | `text/css` | Type of the stylesheets contents, currently only `text/css` is supported. |
 * | `media`   | (empty)    | [unsupported] Specifies a media query that must be matched for the style sheet to apply. |
 * | `title`   | (empty)    | [unsupported] Specifies a title for the style sheet, which is used when selecting between alternate style sheets. |
 *
 * Note that `type` is defined to be a media type [[rfc2046](https://www.ietf.org/rfc/rfc2046.txt)].
 */
// clang-format on
class SVGStyleElement : public SVGElement {
protected:
  /// Create an SVGStyleElement wrapper from an entity.
  explicit SVGStyleElement(EntityHandle handle) : SVGElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Style;
  /// XML tag name, \ref xml_style.
  static constexpr std::string_view Tag{"style"};

  /**
   * Create a new \ref xml_style element.
   *
   * @param document Containing document.
   */
  static SVGStyleElement Create(SVGDocument& document);

  /**
   * Set the type of the stylesheet, currently only `text/css` is supported.
   *
   * @param type Stylesheet type.
   */
  void setType(const RcStringOrRef& type);

  /**
   * Set the contents of the stylesheet.
   *
   * @param style Stylesheet contents (CSS text).
   */
  void setContents(std::string_view style);

  /// Return true if the stylesheet is of type `text/css`.
  bool isCssType() const;
};

}  // namespace donner::svg
