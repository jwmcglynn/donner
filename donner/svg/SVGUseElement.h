#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_use "<use>"
 *
 * `<use>` instantiates another element at a specified position, cloning its rendered output.
 * It's the SVG equivalent of "put another copy of that shape over here" — commonly used for
 * reusable icon libraries, tilesets, symbol sheets, and anywhere you'd otherwise copy-paste
 * the same shape multiple times. Define your artwork once (typically inside `<defs>` or a
 * `<symbol>`), then stamp it out wherever you need it with `<use href="#id" x="..." y="..." />`.
 *
 * The cloned content inherits styles from the `<use>` element itself, so a single symbol can
 * be re-coloured per instance by setting `fill`, `stroke`, etc. on each `<use>`. The clones
 * remain linked to the referenced source and automatically reflect later DOM mutations to the
 * original.
 *
 * - DOM object: SVGUseElement
 * - SVG spec: https://www.w3.org/TR/SVG2/struct.html#UseElement
 *
 * ## Example 1: basic reuse
 *
 * Define a circle once, then stamp a second instance at `x="100"` with a different fill:
 *
 * \htmlonly
 * <svg width="200" height="100" viewBox="0 0 200 100" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <circle id="xml_use_basic_a" cx="50" cy="50" r="40" stroke="blue" fill="none"/>
 *   <use href="#xml_use_basic_a" x="100" fill="blue"/>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <svg width="200" height="100">
 *   <circle id="a" cx="50" cy="50" r="40" stroke="blue" />
 *   <use href="#a" x="100" fill="blue" />
 * </svg>
 * ```
 *
 * ## Example 2: icon library via `<symbol>`
 *
 * A `<symbol>` is a template that isn't rendered on its own — it's only visible through a
 * `<use>` reference. Symbols can declare their own `viewBox`, which makes them resolution
 * independent: the `<use>` element's `width` and `height` decide how big the instance is, and
 * the symbol's `viewBox` is mapped into that box:
 *
 * \htmlonly
 * <svg width="260" height="100" viewBox="0 0 260 100" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <symbol id="xml_use_sym_star" viewBox="0 0 100 100">
 *       <polygon points="50,5 61,39 97,39 68,60 79,95 50,74 21,95 32,60 3,39 39,39" fill="gold" stroke="orange" stroke-width="3"/>
 *     </symbol>
 *   </defs>
 *   <use href="#xml_use_sym_star" x="10" y="20" width="60" height="60"/>
 *   <use href="#xml_use_sym_star" x="90" y="20" width="60" height="60"/>
 *   <use href="#xml_use_sym_star" x="170" y="20" width="60" height="60"/>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <defs>
 *   <symbol id="star" viewBox="0 0 100 100">
 *     <polygon points="50,5 61,39 97,39 68,60 79,95 50,74 21,95 32,60 3,39 39,39"
 *              fill="gold" stroke="orange" stroke-width="3" />
 *   </symbol>
 * </defs>
 * <use href="#star" x="10"  y="20" width="60" height="60" />
 * <use href="#star" x="90"  y="20" width="60" height="60" />
 * <use href="#star" x="170" y="20" width="60" height="60" />
 * ```
 *
 * ## How `preserveAspectRatio` works
 *
 * When `<use>` references a `<symbol>` or nested `<svg>` element that has its own `viewBox`,
 * the `preserveAspectRatio` attribute decides how the referenced content is fitted into the
 * `<use>` element's `width`/`height` box. It answers the question: *"when the symbol's
 * intrinsic aspect ratio doesn't match the destination rectangle, should we stretch,
 * letterbox, or crop?"*
 *
 * The attribute is a pair: `<align> <meetOrSlice>`.
 *
 * - **`<align>`** picks which corner or edge of the source content is aligned to which corner
 *   or edge of the destination when there's extra space. Valid values: `none`, `xMinYMin`,
 *   `xMidYMin`, `xMaxYMin`, `xMinYMid`, `xMidYMid` (the default), `xMaxYMid`, `xMinYMax`,
 *   `xMidYMax`, `xMaxYMax`. The `x*` part controls horizontal alignment (`Min` = left,
 *   `Mid` = center, `Max` = right); the `Y*` part controls vertical alignment (`Min` = top,
 *   `Mid` = middle, `Max` = bottom).
 * - **`<meetOrSlice>`** picks the fitting mode:
 *   - **`meet`** (the default) — scale the content uniformly so it fits *entirely* inside the
 *     destination. If aspect ratios differ, there's empty space on one axis. Think "fit".
 *   - **`slice`** — scale the content uniformly so it *entirely covers* the destination.
 *     If aspect ratios differ, the content is cropped on one axis. Think "fill".
 * - **`none`** as the alignment value disables aspect-ratio preservation entirely: the content
 *   is stretched non-uniformly to exactly fill the destination rectangle. `<meetOrSlice>` is
 *   ignored when alignment is `none`.
 *
 * The default is `xMidYMid meet`.
 *
 * ## Example 3: `preserveAspectRatio` comparison
 *
 * All three instances reference the same square `<symbol>` (`viewBox="0 0 100 100"`) and are
 * sized into a **wider** 120×60 rectangle (dashed outline). Only `preserveAspectRatio` differs:
 *
 * \htmlonly
 * <svg width="420" height="130" viewBox="0 0 420 130" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <symbol id="xml_use_par_sym" viewBox="0 0 100 100">
 *       <rect x="5" y="5" width="90" height="90" fill="#cfe8ff" stroke="#3b82c4" stroke-width="4"/>
 *       <circle cx="50" cy="50" r="30" fill="#3b82c4"/>
 *     </symbol>
 *   </defs>
 *   <g transform="translate(15,15)">
 *     <rect width="120" height="60" fill="none" stroke="#888" stroke-dasharray="3,3"/>
 *     <use href="#xml_use_par_sym" width="120" height="60" preserveAspectRatio="xMidYMid meet"/>
 *     <text x="60" y="85" text-anchor="middle" fill="#444">meet (fit)</text>
 *   </g>
 *   <g transform="translate(150,15)">
 *     <rect width="120" height="60" fill="none" stroke="#888" stroke-dasharray="3,3"/>
 *     <clipPath id="xml_use_par_clip"><rect width="120" height="60"/></clipPath>
 *     <g clip-path="url(#xml_use_par_clip)">
 *       <use href="#xml_use_par_sym" width="120" height="60" preserveAspectRatio="xMidYMid slice"/>
 *     </g>
 *     <text x="60" y="85" text-anchor="middle" fill="#444">slice (fill, cropped)</text>
 *   </g>
 *   <g transform="translate(285,15)">
 *     <rect width="120" height="60" fill="none" stroke="#888" stroke-dasharray="3,3"/>
 *     <use href="#xml_use_par_sym" width="120" height="60" preserveAspectRatio="none"/>
 *     <text x="60" y="85" text-anchor="middle" fill="#444">none (stretched)</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <symbol id="s" viewBox="0 0 100 100"> ... </symbol>
 *
 * <use href="#s" width="120" height="60" preserveAspectRatio="xMidYMid meet"  />
 * <use href="#s" width="120" height="60" preserveAspectRatio="xMidYMid slice" />
 * <use href="#s" width="120" height="60" preserveAspectRatio="none"           />
 * ```
 *
 * Note: `preserveAspectRatio` only has an effect when the referenced element has an intrinsic
 * coordinate system (a `viewBox`). Referencing a bare `<circle>` or `<path>` ignores it.
 *
 * ## Attributes
 *
 * | Attribute             | Default         | Description |
 * | --------------------: | :-------------: | :---------- |
 * | `href`                | (none)          | URI (typically `#id`) of the element to reuse. |
 * | `x`                   | `0`             | X coordinate where the referenced element is placed. |
 * | `y`                   | `0`             | Y coordinate where the referenced element is placed. |
 * | `width`               | `auto`          | Width of the instance. Only meaningful when the referenced element has a `viewBox` (e.g. `<symbol>` or `<svg>`). |
 * | `height`              | `auto`          | Height of the instance. Only meaningful when the referenced element has a `viewBox`. |
 * | `preserveAspectRatio` | `xMidYMid meet` | How the referenced content is fitted into `width` × `height` when aspect ratios differ. See above. |
 */

/**
 * DOM object for a \ref xml_use element.
 *
 * ```xml
 * <svg width="200" height="100">
 *   <circle id="a" cx="50" cy="50" r="40" stroke="blue" />
 *   <use href="#a" x="100" fill="blue" />
 * </svg>
 * ```
 * \htmlonly
 * <svg width="200" height="100">
 *   <circle id="a" cx="50" cy="50" r="40" stroke="blue" />
 *   <use href="#a" x="100" fill="blue" />
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `x`       | `0`     | X coordinate to position the referenced element. |
 * | `y`       | `0`     | Y coordinate to position the referenced element. |
 * | `width`   | `auto`  | Width of the referenced element. |
 * | `height`  | `auto`  | Height of the referenced element. |
 * | `href`    | (none)  | URI to the element to reuse. |
 */
class SVGUseElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGUseElement object.
  explicit SVGUseElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGUseElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Use;
  /// XML tag name, \ref xml_use.
  static constexpr std::string_view Tag{"use"};

  /**
   * Create a new \ref xml_use element.
   *
   * @param document Containing document.
   */
  static SVGUseElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

  /**
   * Set the URI to the element to reuse.
   *
   * @param value URI to the element to reuse, such as `#elementId`
   */
  void setHref(const RcString& value);

  /**
   * Set the X coordinate to position the referenced element.
   *
   * @param value X coordinate to position the referenced element.
   */
  void setX(Lengthd value);

  /**
   * Set the Y coordinate to position the referenced element.
   *
   * @param value Y coordinate to position the referenced element.
   */
  void setY(Lengthd value);

  /**
   * Set the width to scale the referenced element.
   *
   * @param value Width to scale the referenced element.
   */
  void setWidth(std::optional<Lengthd> value);

  /**
   * Set the height to scale the referenced element.
   *
   * @param value Height to scale the referenced element.
   */
  void setHeight(std::optional<Lengthd> value);

  /**
   * Get the URI to the element to reuse.
   */
  RcString href() const;

  /**
   * Get the X coordinate to position the referenced element.
   */
  Lengthd x() const;

  /**
   * Get the Y coordinate to position the referenced element.
   */
  Lengthd y() const;

  /**
   * Get the width to scale the referenced element.
   */
  std::optional<Lengthd> width() const;

  /**
   * Get the height to scale the referenced element.
   */
  std::optional<Lengthd> height() const;
};

}  // namespace donner::svg
