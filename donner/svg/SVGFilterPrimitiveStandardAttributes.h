#pragma once
/// @file

#include "donner/base/RcStringOrRef.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * Handles standard attributes for filter primitives, which are children of the \ref xml_filter
 * element.
 *
 * @see https://www.w3.org/TR/filter-effects/#InterfaceSVGFilterPrimitiveStandardAttributes
 */
class SVGFilterPrimitiveStandardAttributes : public SVGElement {
protected:
  /// Inheriting constructor to be called by derived classes. \ref
  /// SVGFilterPrimitiveStandardAttributes cannot be instantiated directly.
  explicit SVGFilterPrimitiveStandardAttributes(EntityHandle handle);

public:
  /**
   * Get the top-left X coordinate of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '-10%'.
   */
  Lengthd x() const;

  /**
   * Get the top-left Y coordinate of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '-10%'.
   */
  Lengthd y() const;

  /**
   * Get the width of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '120%'.
   */
  Lengthd width() const;

  /**
   * Get the height of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '120%'.
   */
  Lengthd height() const;

  /**
   * Set the top-left X coordinate of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '-10%'.
   *
   * @param value Coordinate value.
   */
  void setX(const Lengthd& value);

  /**
   * Set the top-left Y coordinate of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '-10%'.
   *
   * @param value Coordinate value.
   */
  void setY(const Lengthd& value);

  /**
   * Set the width of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '120%'.
   *
   * @param value Dimension value.
   */
  void setWidth(const Lengthd& value);

  /**
   * Set the height of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '120%'.
   *
   * @param value Dimension value.
   */
  void setHeight(const Lengthd& value);

  /**
   * Get the name of this filter primitive, which enables it to be referenced by a subsequent filter
   * primitive within the same filter element. If no value is provided, the output will only be
   * available for re-use as the implicit input into the next filter primitive if that filter
   * primitive provides no value for its in attribute.
   */
  std::optional<RcString> result() const;

  /**
   * Set the name of this filter primitive, which enables it to be referenced by a subsequent filter
   * primitive within the same filter element. If no value is provided, the output will only be
   * available for re-use as the implicit input into the next filter primitive if that filter
   * primitive provides no value for its in attribute.
   */
  void setResult(const RcStringOrRef& value);
};

}  // namespace donner::svg
