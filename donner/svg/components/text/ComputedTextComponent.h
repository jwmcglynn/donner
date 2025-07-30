#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"

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

    /// Byte index (inclusive) of the first code unit of the span within \c text.
    std::size_t start;
    /// Byte index (exclusive) one past the last code unit of the span within \c text.
    std::size_t end;
    /// Absolute X position for the first glyph of the span.
    Lengthd x;
    /// Absolute Y baseline position for the span.
    Lengthd y;
    /// Relative X shift applied to the span.
    Lengthd dx;
    /// Relative Y shift applied to the span.
    Lengthd dy;
    /// Rotation applied to each glyph in the span (degrees).
    double rotateDegrees = 0.0;
  };

  // Computed spans with positioning data for rendering.
  SmallVector<TextSpan, 1> spans;
};

}  // namespace donner::svg::components
