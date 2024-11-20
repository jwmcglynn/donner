#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_use "<use>"
 *
 * Reuses an existing element by referencing it with a URI. This is useful for creating
 * repeating patterns or reusing complex shapes.
 *
 * - DOM object: SVGUseElement
 * - SVG spec: https://www.w3.org/TR/SVG2/struct.html#UseElement
 *
 * The `<use>` element references another element, a copy of which is rendered in place of the
 * `<use>` in the document. The referenced element may be a container element, in which case a copy
 * of the complete SVG document subtree rooted at that element is used.
 *
 * The cloned content inherits styles from the `<use>` element. However, these cloned element
 * instances remain linked to the referenced source and reflect DOM mutations in the original.
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
  static SVGUseElement Create(SVGDocument& document) { return CreateOn(CreateEntity(document)); }

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
