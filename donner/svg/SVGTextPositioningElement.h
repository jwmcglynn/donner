#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/base/SmallVector.h"
#include "donner/svg/SVGTextContentElement.h"

namespace donner::svg {

/**
 * Base class for elements that support per-glyph positioning attributes (`x`, `y`, `dx`, `dy`,
 * `rotate`) on top of the basic text content interface. This corresponds to the W3C IDL
 * interface `SVGTextPositioningElement`.
 *
 * These attributes can contain a list of values, one for each character. The methods here provide
 * access to either the first value in the list (for convenience) or the entire list.
 *
 * \see https://www.w3.org/TR/SVG2/text.html#InterfaceSVGTextPositioningElement
 */
class SVGTextPositioningElement : public SVGTextContentElement {
  friend class parser::SVGParserImpl;

protected:
  /**
   * Inheriting constructor to be called by derived classes. \ref SVGTextPositioningElement cannot
   * be instantiated directly.
   *
   * @param handle The handle to the underlying entity.
   */
  explicit SVGTextPositioningElement(EntityHandle handle);

public:
  /// Returns true if the given element type can be cast to \ref SVGTextPositioningElement.
  static constexpr bool IsBaseOf(ElementType type) {
    return type == ElementType::Text || type == ElementType::TSpan;
  }

  /**
   * Sets the `x` attribute list to a single value (absolute x-position).
   * Any existing values in the list are replaced.
   *
   * @param value Coordinate value, or \c std::nullopt to clear the list.
   */
  void setX(std::optional<Lengthd> value);

  /**
   * Gets the first `x` attribute value (absolute x-position) from the list.
   *
   * @return The first coordinate value if the list is not empty, otherwise \c std::nullopt.
   */
  std::optional<Lengthd> x() const;

  /**
   * Sets the `x` attribute list (absolute x-position for each character).
   *
   * @param value List of coordinate values.
   */
  void setXList(SmallVector<Lengthd, 1>&& value);

  /**
   * Gets the `x` attribute list (absolute x-position for each character).
   *
   * @return Const reference to the list of coordinate values.
   */
  const SmallVector<Lengthd, 1>& xList() const;

  /**
   * Sets the `y` attribute list to a single value (absolute y-position).
   * Any existing values in the list are replaced.
   *
   * @param value Coordinate value, or \c std::nullopt to clear the list.
   */
  void setY(std::optional<Lengthd> value);

  /**
   * Gets the first `y` attribute value (absolute y-position) from the list.
   *
   * @return The first coordinate value if the list is not empty, otherwise \c std::nullopt.
   */
  std::optional<Lengthd> y() const;

  /**
   * Sets the `y` attribute list (absolute y-position for each character).
   *
   * @param value List of coordinate values.
   */
  void setYList(SmallVector<Lengthd, 1>&& value);

  /**
   * Gets the `y` attribute list (absolute y-position for each character).
   *
   * @return Const reference to the list of coordinate values.
   */
  const SmallVector<Lengthd, 1>& yList() const;

  /**
   * Sets the `dx` attribute list to a single value (relative shift in x).
   * Any existing values in the list are replaced.
   *
   * @param value Coordinate value, or \c std::nullopt to clear the list.
   */
  void setDx(std::optional<Lengthd> value);

  /**
   * Gets the first `dx` attribute value (relative shift in x) from the list.
   *
   * @return The first coordinate value if the list is not empty, otherwise \c std::nullopt.
   */
  std::optional<Lengthd> dx() const;

  /**
   * Sets the `dx` attribute list (relative shift in x for each character).
   *
   * @param value List of coordinate values.
   */
  void setDxList(SmallVector<Lengthd, 1>&& value);

  /**
   * Gets the `dx` attribute list (relative shift in x for each character).
   *
   * @return Const reference to the list of coordinate values.
   */
  const SmallVector<Lengthd, 1>& dxList() const;

  /**
   * Sets the `dy` attribute list to a single value (relative shift in y).
   * Any existing values in the list are replaced.
   *
   * @param value Coordinate value, or \c std::nullopt to clear the list.
   */
  void setDy(std::optional<Lengthd> value);

  /**
   * Gets the first `dy` attribute value (relative shift in y) from the list.
   *
   * @return The first coordinate value if the list is not empty, otherwise \c std::nullopt.
   */
  std::optional<Lengthd> dy() const;

  /**
   * Sets the `dy` attribute list (relative shift in y for each character).
   *
   * @param value List of coordinate values.
   */
  void setDyList(SmallVector<Lengthd, 1>&& value);

  /**
   * Gets the `dy` attribute list (relative shift in y for each character).
   *
   * @return Const reference to the list of coordinate values.
   */
  const SmallVector<Lengthd, 1>& dyList() const;

  /**
   * Sets the `rotate` attribute list to a single value (rotation in degrees).
   * Any existing values in the list are replaced.
   *
   * @param degrees Rotation in degrees, or \c std::nullopt to clear the list.
   */
  void setRotate(std::optional<double> degrees);

  /**
   * Gets the first `rotate` attribute value from the list.
   *
   * @return Rotation in degrees if the list is not empty, otherwise \c std::nullopt.
   */
  std::optional<double> rotate() const;

  /**
   * Sets the `rotate` attribute list (rotation in degrees for each character).
   *
   * @param value List of rotation values in degrees.
   */
  void setRotateList(SmallVector<double, 1>&& value);

  /**
   * Gets the `rotate` attribute list (rotation in degrees for each character).
   *
   * @return Const reference to the list of rotation values in degrees.
   */
  const SmallVector<double, 1>& rotateList() const;
};

}  // namespace donner::svg
