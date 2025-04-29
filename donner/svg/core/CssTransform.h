#pragma once
/// @file

#include <variant>
#include <vector>

#include "donner/base/Length.h"
#include "donner/base/Transform.h"

namespace donner::svg {

/**
 * Compared to an SVG transform, CSS transforms have additional features, such as the ability to add
 * units to the `translate()` function, such as `translate(1em 30px)`.
 *
 * ```
 * translate() = translate( <length-percentage> [, <length-percentage> ]? )
 * ```
 *
 * To resolve `translate()`, we need to know the font size and the viewBox size, which is
 * context-dependent, so we cannot precompute the transform from the transform function list.
 * Instead, store a chain of of transforms and deferred operations, and compute the final transform
 * when needed, inside the \ref CssTransform::compute() function.
 *
 * See https://www.w3.org/TR/css-transforms-1/#two-d-transform-functions for more details about CSS
 * transforms.
 *
 * CssTransform is parsed by \ref donner::svg::parser::CssTransformParser.
 */
class CssTransform {
public:
  /**
   * Construct an empty transform set to identity.
   */
  CssTransform() = default;

  /**
   * Construct a transform initialized with the given transform.
   *
   * @param transform Initial transform.
   */
  explicit CssTransform(const Transformd& transform) { appendTransform(transform); }

  /**
   * Stores a precomputed transform.
   */
  struct Simple {
    Transformd transform;  ///< Transform to apply.
  };

  /**
   * Stores a deferred `translate()` operation, which can have two `<length-percentage>` arguments,
   * such as `translate(1em 30px)`.
   */
  struct Translate {
    Lengthd x;  ///< X offset.
    Lengthd y;  ///< Y offset.
  };

  /// A transform or a deferred operation.
  using Element = std::variant<Simple, Translate>;

  /**
   * Compute the final transform from the list of transforms and deferred operations.
   *
   * @param viewBox ViewBox size, used to resolve percentage units.
   * @param fontMetrics Font metrics, used to resolve 'em' and other font-relative units.
   */
  Transformd compute(const Boxd& viewBox, FontMetrics fontMetrics) const {
    Transformd result;
    for (const auto& element : elements_) {
      if (const auto* simple = std::get_if<Simple>(&element)) {
        result = simple->transform * result;
      } else if (const auto* translate = std::get_if<Translate>(&element)) {
        result = Transformd::Translate(
                     Vector2d(translate->x.toPixels(viewBox, fontMetrics, Lengthd::Extent::X),
                              translate->y.toPixels(viewBox, fontMetrics, Lengthd::Extent::Y))) *
                 result;
      }
    }

    return result;
  }

  /**
   * Append a transform to the transform chain.
   *
   * @param transform Transform to append.
   */
  void appendTransform(const Transformd& transform) {
    if (!elements_.empty()) {
      if (Simple* s = std::get_if<Simple>(&elements_.back())) {
        s->transform = s->transform * transform;
        return;
      }
    }

    elements_.emplace_back(Simple{transform});
  }

  /**
   * Append a `translate()` operation to the transform chain.
   *
   * @param x X offset.
   * @param y Y offset.
   */
  void appendTranslate(Lengthd x, Lengthd y) { elements_.emplace_back(Translate{x, y}); }

private:
  std::vector<Element> elements_;
};

}  // namespace donner::svg
