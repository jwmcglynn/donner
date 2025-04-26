#pragma once
/// @file

#include <optional>
#include <string_view>

#include "donner/base/OptionalRef.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg {

/**
 * @page xml_symbol "<symbol>"
 *
 * Defines a symbol element that can be used to define graphical templates which are not rendered
 * directly but can be instantiated by a \ref xml_use element. A symbol element establishes a nested
 * coordinate system for the graphics it contains. When the symbol is referenced by a \ref xml_use
 * element, its content is rendered similarly to a nested \ref xml_svg element.
 *
 * - DOM object: SVGSymbolElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/struct.html#SymbolElement
 *
 * ```xml
 * <symbol id="icon" viewBox="0 0 100 100" preserveAspectRatio="xMidYMid meet"
 *         x="0" y="0" width="100" height="100" refX="50" refY="50">
 *   <circle cx="50" cy="50" r="40" fill="blue" />
 * </symbol>
 * ```
 *
 * \htmlonly
 * <svg width="300" height="300" xmlns="http://www.w3.org/2000/svg" style="background-color: white">
 *   <defs>
 *     <symbol id="icon" viewBox="0 0 100 100" preserveAspectRatio="xMidYMid meet"
 *             x="0" y="0" width="100" height="100" refX="50" refY="50">
 *       <circle cx="50" cy="50" r="40" fill="blue" />
 *     </symbol>
 *   </defs>
 *
 *   <use href="#icon" x="50" y="50" width="200" height="200" />
 * </svg>
 * \endhtmlonly
 *
 * | Attribute             | Default         | Description |
 * | ---------------------:| :-------------: | :---------- |
 * | `viewBox`             | (none)          | A list of four numbers (min-x, min-y, width, height) separated by whitespace and/or commas that specify a rectangle in user space which is mapped to the symbol's viewport. |
 * | `preserveAspectRatio` | `xMidYMid meet` | Determines how the symbol's contents are scaled to fit the viewport defined by `viewBox`. Only applies if `viewBox` is specified. |
 * | `x`                   | `0`             | The x coordinate of the symbol's viewport. |
 * | `y`                   | `0`             | The y coordinate of the symbol's viewport. |
 * | `width`               | `auto`          | The width of the symbol's viewport. A value of auto is interpreted as 100% when instantiated. |
 * | `height`              | `auto`          | The height of the symbol's viewport. A value of auto is interpreted as 100% when instantiated. |
 * | `refX`                | `0`             | The reference x coordinate used when the symbol is instantiated via a \ref xml_use element. |
 * | `refY`                | `0`             | The reference y coordinate used when the symbol is instantiated via a \ref xml_use element. |
 */

/**
 * DOM object for a \ref xml_symbol element, which defines a graphical template that can be
 * instantiated using a \ref xml_use element. The symbol element itself is not rendered directly;
 * instead, its contents are rendered when referenced.
 */
class SVGSymbolElement : public SVGElement {
  friend class parser::SVGParserImpl;

private:
  /// Create an SVGSymbolElement wrapper from an entity.
  explicit SVGSymbolElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing entity.
   *
   * @param handle Entity handle.
   */
  static SVGSymbolElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Symbol;
  /// XML tag name, \ref xml_symbol.
  static constexpr std::string_view Tag{"symbol"};

  /**
   * Create a new \ref xml_symbol element.
   *
   * @param document Containing document.
   */
  static SVGSymbolElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

  /**
   * Set the `viewBox` attribute, which defines a rectangle in user space
   * that is mapped to the symbol's viewport.
   *
   * @param viewbox The viewBox value to set.
   */
  void setViewbox(OptionalRef<Boxd> viewbox);

  /**
   * Get the parsed value of the `viewBox` attribute, if specified.
   *
   * @return The viewBox rectangle, if set.
   */
  std::optional<Boxd> viewbox() const;

  /**
   * Set the `preserveAspectRatio` attribute, which determines how the
   * symbol’s viewport scales its content.
   *
   * @param preserveAspectRatio The preserveAspectRatio value to set.
   */
  void setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio);

  /**
   * Get the value of the `preserveAspectRatio` attribute.
   *
   * @return The preserveAspectRatio setting.
   */
  PreserveAspectRatio preserveAspectRatio() const;

  /**
   * Set the x coordinate of the symbol.
   *
   * @param value The x coordinate.
   */
  void setX(Lengthd value);

  /**
   * Get the x coordinate of the symbol.
   *
   * @return The x coordinate.
   */
  Lengthd x() const;

  /**
   * Set the y coordinate of the symbol.
   *
   * @param value The y coordinate.
   */
  void setY(Lengthd value);

  /**
   * Get the y coordinate of the symbol.
   *
   * @return The y coordinate.
   */
  Lengthd y() const;

  /**
   * Set the width of the symbol, or \c std::nullopt for 'auto'.
   *
   * @param value The width of the symbol.
   */
  void setWidth(std::optional<Lengthd> value);

  /**
   * Get the width of the symbol, or \c std::nullopt for 'auto'.
   *
   * @return The width of the symbol.
   */
  std::optional<Lengthd> width() const;

  /**
   * Set the height of the symbol, or \c std::nullopt for 'auto'.
   *
   * @param value The height of the symbol.
   */
  void setHeight(std::optional<Lengthd> value);

  /**
   * Get the height of the symbol, or \c std::nullopt for 'auto'.
   *
   * @return The height of the symbol.
   */
  std::optional<Lengthd> height() const;

  /**
   * Set the reference x coordinate.
   *
   * @param value The reference x coordinate.
   */
  void setRefX(double value);

  /**
   * Get the reference x coordinate.
   *
   * @return The reference x coordinate.
   */
  double refX() const;

  /**
   * Set the reference y coordinate.
   *
   * @param value The reference y coordinate.
   */
  void setRefY(double value);

  /**
   * Get the reference y coordinate.
   *
   * @return The reference y coordinate.
   */
  double refY() const;
};

}  // namespace donner::svg
