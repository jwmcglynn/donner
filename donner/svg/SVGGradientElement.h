#pragma once
/// @file

#include "donner/base/Transform.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/core/Gradient.h"

namespace donner::svg {

/**
 * Base class for SVG gradient elements, such as \ref SVGLinearGradientElement and \ref
 * SVGRadialGradientElement.
 *
 * This stores common attributes for gradients, such as `gradientUnits` and `spreadMethod`, and also
 * supports inheriting attributes from other gradients with the `href` attribute.
 *
 * \see https://www.w3.org/TR/SVG2/pservers.html#InterfaceSVGGradientElement
 *
 * | Attribute | Default | Description |
 * | --------: | :-----: | :---------- |
 * | `gradientUnits` | `objectBoundingBox` | The coordinate system for the gradient, either `userSpaceOnUse` or `objectBoundingBox`. |
 * | `gradientTransform` | (none) | A transform to apply to the gradient. |
 * | `spreadMethod` | `pad` | How to handle colors outside the gradient. Either `pad`, `reflect`, or `repeat`. |
 * | `href`    | (none)  | A URL reference to a template gradient element, which is then used as a template for this gradient. Example: `<linearGradient id="MyGradient" href="#MyGradient2" />` |
 */
class SVGGradientElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /**
   * Constructor for SVGGradientElement, which must be constructed from a derived class.
   *
   * @param handle The handle to the underlying entity.
   */
  explicit SVGGradientElement(EntityHandle handle);

public:
  /**
   * `href` attribute to allow inheriting attributes from another gradient.
   *
   * ```xml
   * <linearGradient id="MyGradient" x1="0" y1="0" x2="1" y2="0">
   *   <stop offset="0%" stop-color="blue" />
   *   <stop offset="100%" stop-color="yellow" />
   * </linearGradient>
   *
   * <linearGradient id="MyGradient2" href="#MyGradient" gradientTransform="rotate(45deg)">
   *  <!-- Stops are inherited from MyGradient -->
   * </linearGradient>
   * ```
   *
   * \htmlonly
   * <svg height="300" width="620">
   *   <style>
   *     text { font-size: 16px; font-weight: bold; color: black; text-anchor: middle }
   *   </style>
   *   <linearGradient id="MyGradient" x1="0" y1="0" x2="1" y2="0">
   *     <stop offset="0%" stop-color="blue" />
   *     <stop offset="100%" stop-color="yellow" />
   *   </linearGradient>
   *   <linearGradient id="MyGradient2" href="#MyGradient" gradientTransform="rotate(45)">
   *     <!-- Stops are inherited from MyGradient -->
   *   </linearGradient>
   *   <rect fill="url(#MyGradient)" width="300" height="300" />
   *   <text x="150" y="280">Original</text>
   *   <rect fill="url(#MyGradient2)" x="320" width="300" height="300" />
   *   <text x="470" y="280">Inherited with rotate(45)</text>
   * </svg>
   * \endhtmlonly
   *
   * The attributes that can be inherited are:
   * - `gradientUnits`, `spreadMethod`, and `gradientTransform`.
   * - For \ref SVGLinearGradientElement, `x1`, `y1`, `x2`, and `y2`.
   * - For \ref SVGRadialGradientElement, `cx`, `cy`, `r`, `fx`, `fy`, and `fr`.
   * - \ref xml_stop child elements, if this element has none itself.
   *
   * \see https://www.w3.org/TR/SVG2/pservers.html#LinearGradientElementHrefAttribute
   * \see https://www.w3.org/TR/SVG2/pservers.html#RadialGradientElementHrefAttribute
   *
   * @returns A URL reference to a template gradient element; to be valid, the reference must be to
   *   a different \ref xml_linearGradient or a \ref xml_radialGradient element.
   */
  std::optional<RcString> href() const;

  /**
   * `gradientUnits` attribute to specify the coordinate system for the gradient.
   *
   * The default is \ref GradientUnits::ObjectBoundingBox, where (0, 0) is the top-left corner of
   * the element that references the gradient, and (1, 1) is the bottom-right corner.
   *
   * This affects the following attributes:
   * - For \ref SVGLinearGradientElement, `x1`, `y1`, `x2`, and `y2`.
   * - For \ref SVGRadialGradientElement, `cx`, `cy`, `r`, `fx`, `fy`, and `fr`.
   */
  GradientUnits gradientUnits() const;

  /**
   * `gradientTransform` attribute to specify a transform to apply to the gradient.
   *
   * The default is the identity transform.
   */
  Transformd gradientTransform() const;

  /**
   * `spreadMethod` attribute to specify how to fill the area outside the gradient.
   *
   * The default is \ref GradientSpreadMethod::Pad, which fills with the start or end color.
   *
   * \copydoc GradientSpreadMethod
   */
  GradientSpreadMethod spreadMethod() const;

  /**
   * `href` attribute to allow inheriting attributes from another gradient.
   *
   * \see \ref href()
   *
   * @param value URL reference such as `"#otherId"` to a template gradient element, or
   *   `std::nullopt` to remove the attribute. To be valid, the reference must be to a different
   *   \ref xml_linearGradient or a \ref xml_radialGradient element.
   */
  void setHref(const std::optional<RcString>& value);

  /**
   * `gradientUnits` attribute to specify the coordinate system for the gradient.
   *
   * \see \ref gradientUnits()
   *
   * @param value The coordinate system for the gradient.
   */
  void setGradientUnits(GradientUnits value);

  /**
   * `gradientTransform` attribute to specify a transform to apply to the gradient.
   *
   * \see \ref gradientTransform()
   *
   * @param value The transform to apply to the gradient.
   */
  void setGradientTransform(const Transformd& value);

  /**
   * `spreadMethod` attribute to specify how to fill the area outside the gradient.
   *
   * \see \ref spreadMethod()
   *
   * @param value The method to use to fill the area outside the gradient.
   */
  void setSpreadMethod(GradientSpreadMethod value);
};

}  // namespace donner::svg
