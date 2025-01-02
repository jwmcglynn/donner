#pragma once
/// @file

#include "donner/base/RcStringOrRef.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @defgroup xml_style "<style>"
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
 * \htmlonly
 * <svg xmlns="http://www.w3.org/2000/svg" viewBox="20 20 750 290">
 *   <style>
 *     .code-box { fill: #f8f9fa; stroke: #dee2e6; stroke-width: 2; }
 *     .code-text { font-family: monospace; font-size: 14px; fill: #212529; }
 *     .rect { fill: crimson; stroke: darkred; stroke-width: 3; }
 *     circle:nth-child(3n+1) { fill: red; }
 *     circle:nth-child(3n+2) { fill: limegreen; }
 *     circle:nth-child(3n+3) { fill: blue; }
 *   </style>
 *
 *   <!-- Code Example Box -->
 *   <rect x="40" y="40" width="430" height="250" class="code-box" rx="4"/>
 *   <text x="60" y="70" class="code-text">&lt;style&gt;</text>
 *   <text x="60" y="90" class="code-text">&#160;&#160;.rect { </text>
 *   <text x="60" y="110" class="code-text">&#160;&#160;&#160;&#160;fill: crimson;</text>
 *   <text x="60" y="130" class="code-text">&#160;&#160;&#160;&#160;stroke: darkred;</text>
 *   <text x="60" y="150" class="code-text">&#160;&#160;}</text>
 *   <text x="60" y="180" class="code-text">&#160;&#160;.circle:nth-child(3n+1) { fill: red; }</text>
 *   <text x="60" y="210" class="code-text">&#160;&#160;.circle:nth-child(3n+2) { fill: limegreen; }</text>
 *   <text x="60" y="240" class="code-text">&#160;&#160;.circle:nth-child(3n+3) { fill: blue; }</text>
 *   <text x="60" y="270" class="code-text">&lt;/style&gt;</text>
 *
 *   <!-- Visual Demo -->
 *   <rect x="490" y="40" width="260" height="250" class="code-box" rx="4"/>
 *   <rect x="560" y="120" width="120" height="40" class="rect"/>
 *   <g>
 *     <circle cx="540" cy="200" r="15" />
 *     <circle cx="580" cy="200" r="15" />
 *     <circle cx="620" cy="200" r="15" />
 *     <circle cx="660" cy="200" r="15" />
 *     <circle cx="700" cy="200" r="15" />
 *   </g>
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
 *
 * ## Children
 *
 * Either text or CDATA nodes are allowed as child nodes.
 */

/**
 * DOM object for a \ref xml_style element, which contains a CSS stylesheet.
 *
 * \htmlonly
 * <svg xmlns="http://www.w3.org/2000/svg" viewBox="20 20 750 290">
 *   <style>
 *     .code-box { fill: #f8f9fa; stroke: #dee2e6; stroke-width: 2; }
 *     .code-text { font-family: monospace; font-size: 14px; fill: #212529; }
 *     .rect { fill: crimson; stroke: darkred; stroke-width: 3; }
 *     circle:nth-child(3n+1) { fill: red; }
 *     circle:nth-child(3n+2) { fill: limegreen; }
 *     circle:nth-child(3n+3) { fill: blue; }
 *   </style>
 *
 *   <!-- Code Example Box -->
 *   <rect x="40" y="40" width="430" height="250" class="code-box" rx="4"/>
 *   <text x="60" y="70" class="code-text">&lt;style&gt;</text>
 *   <text x="60" y="90" class="code-text">&#160;&#160;.rect { </text>
 *   <text x="60" y="110" class="code-text">&#160;&#160;&#160;&#160;fill: crimson;</text>
 *   <text x="60" y="130" class="code-text">&#160;&#160;&#160;&#160;stroke: darkred;</text>
 *   <text x="60" y="150" class="code-text">&#160;&#160;}</text>
 *   <text x="60" y="180" class="code-text">&#160;&#160;.circle:nth-child(3n+1) { fill: red; }</text>
 *   <text x="60" y="210" class="code-text">&#160;&#160;.circle:nth-child(3n+2) { fill: limegreen; }</text>
 *   <text x="60" y="240" class="code-text">&#160;&#160;.circle:nth-child(3n+3) { fill: blue; }</text>
 *   <text x="60" y="270" class="code-text">&lt;/style&gt;</text>
 *
 *   <!-- Visual Demo -->
 *   <rect x="490" y="40" width="260" height="250" class="code-box" rx="4"/>
 *   <rect x="560" y="120" width="120" height="40" class="rect"/>
 *   <g>
 *     <circle cx="540" cy="200" r="15" />
 *     <circle cx="580" cy="200" r="15" />
 *     <circle cx="620" cy="200" r="15" />
 *     <circle cx="660" cy="200" r="15" />
 *     <circle cx="700" cy="200" r="15" />
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default    | Description  |
 * | --------: | :--------: | :----------- |
 * | `type`    | `text/css` | Type of the stylesheets contents, currently only `text/css` is supported. Use \ref SVGStyleElement::isCssType() to check. |
 * | `media`   | (empty)    | [unsupported] Specifies a media query that must be matched for the style sheet to apply. |
 * | `title`   | (empty)    | [unsupported] Specifies a title for the style sheet, which is used when selecting between alternate style sheets. |
 *
 * Note that `type` is defined to be a media type [[rfc2046](https://www.ietf.org/rfc/rfc2046.txt)].
 *
 * ## Setting Style
 *
 * Use \ref SVGStyleElement::setContents and pass a CSS stylesheet string.
 */
class SVGStyleElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGStyleElement wrapper from an entity.
  explicit SVGStyleElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGStyleElement CreateOn(EntityHandle handle);

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
  static SVGStyleElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

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
