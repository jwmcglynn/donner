#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/svg/components/text/ComputedTextStyleComponent.h"

namespace donner::svg::components {

/**
 * Pre‑computed layout information for a text subtree.
 *
 * A **ComputedTextComponent** is attached by the layout system to the root \ref xml_text element
 * after all \ref xml_text, \ref xml_tspan, and \ref xml_textPath descendants have been resolved.
 * It stores the final, absolute positions for each contiguous slice of text, allowing the renderer
 * to iterate quickly without re‑evaluating attribute vectors on every frame.
 *
 * The component contains a single public field, \ref spans, which is the computed list of text
 * spans.
 *
 * \note This structure is internal to the rendering pipeline and is not exposed through the
 *       public DOM API.
 */
struct ComputedTextComponent {
  /**
   * A contiguous slice of text with fully resolved layout attributes.
   *
   * Offsets \c start and \c end refer to UTF-8 byte positions into the parent
   * string stored in \c text.  All coordinate and rotation values are given in
   * the user coordinate system and already include any inherited transformations
   * from ancestor \ref xml_text elements.
   */
  struct TextSpan {
    /// Back‑reference to the original text for this span.
    RcString text;

    /// Typography resolved for this span at layout time.
    ComputedTextStyleComponent style;

    /// Byte index (inclusive) of the first code unit of the span within \c text.
    std::size_t start;
    /// Byte index (exclusive) one past the last code unit of the span within \c text.
    std::size_t end;
    /// Absolute X positions (per-glyph positioning). If empty, use default flow.
    SmallVector<Lengthd, 1> x;
    /// Absolute Y baseline positions (per-glyph positioning). If empty, use default flow.
    SmallVector<Lengthd, 1> y;
    /// Relative X shifts (per-glyph). If empty, no relative shift.
    SmallVector<Lengthd, 1> dx;
    /// Relative Y shifts (per-glyph). If empty, no relative shift.
    SmallVector<Lengthd, 1> dy;
    /// Rotation applied to each glyph in the span (degrees).
    double rotateDegrees = 0.0;
  };

  // Computed spans with positioning data for rendering.
  SmallVector<TextSpan, 1> spans;
};

}  // namespace donner::svg::components
