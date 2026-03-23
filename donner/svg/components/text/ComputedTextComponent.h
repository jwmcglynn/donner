#pragma once
/// @file

#include <optional>

#include "donner/css/Color.h"
#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/svg/core/DominantBaseline.h"
#include "donner/svg/core/PathSpline.h"

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
    /// True when \ref x should reset the current text position for this span.
    bool hasX = false;
    /// Absolute Y baseline position for the span.
    Lengthd y;
    /// True when \ref y should reset the current text position for this span.
    bool hasY = false;
    /// Relative X shift applied to the span.
    Lengthd dx;
    /// True when \ref dx should shift the current text position for this span.
    bool hasDx = false;
    /// Relative Y shift applied to the span.
    Lengthd dy;
    /// True when \ref dy should shift the current text position for this span.
    bool hasDy = false;
    /// Rotation applied to each glyph in the span (degrees).
    double rotateDegrees = 0.0;

    /// Resolved solid fill color for this span, if one is available from computed style.
    std::optional<css::Color> fillColor;

    /// CSS font-weight for this span (100-900, 400=normal, 700=bold).
    int fontWeight = 400;

    /// Per-character absolute X positions from \c x attribute lists. Indexed by character
    /// (codepoint) index within the span. \c nullopt means no explicit position — the glyph
    /// advances naturally from the previous character.
    std::vector<std::optional<Lengthd>> xList;
    /// Per-character absolute Y positions from \c y attribute lists.
    std::vector<std::optional<Lengthd>> yList;
    /// Per-character relative X offsets from \c dx attribute lists.
    std::vector<std::optional<Lengthd>> dxList;
    /// Per-character relative Y offsets from \c dy attribute lists.
    std::vector<std::optional<Lengthd>> dyList;
    /// Per-character rotation in degrees from \c rotate attribute lists. Per SVG spec, the
    /// last value repeats for all subsequent characters beyond the list length.
    std::vector<double> rotateList;

    /// CSS `baseline-shift` value for this span. Stored as a Lengthd where em units are relative
    /// to the span's font-size. Positive = shift up (per CSS convention). Resolved to pixels in
    /// the layout engine.
    Lengthd baselineShift;

    /// CSS `alignment-baseline` value for this span. When not Auto, overrides the
    /// dominant-baseline for this specific inline element.
    DominantBaseline alignmentBaseline = DominantBaseline::Auto;

    /// If set, glyphs in this span are positioned along this path (for \ref xml_textPath).
    std::optional<PathSpline> pathSpline;
    /// Start offset along the path (resolved to pixels).
    double pathStartOffset = 0.0;
  };

  // Computed spans with positioning data for rendering.
  SmallVector<TextSpan, 1> spans;
};

}  // namespace donner::svg::components
