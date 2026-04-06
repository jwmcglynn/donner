#pragma once
/// @file

#include <entt/entt.hpp>
#include <optional>

#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/core/DominantBaseline.h"
#include "donner/svg/core/FontStretch.h"
#include "donner/svg/core/FontStyle.h"
#include "donner/svg/core/FontVariant.h"
#include "donner/svg/core/LengthAdjust.h"
#include "donner/svg/core/PathSpline.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/core/TextAnchor.h"
#include "donner/svg/core/TextDecoration.h"
#include "donner/svg/core/Visibility.h"

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

    /// Back-reference to the source entity (text, tspan, or textPath) that produced this span.
    /// Used by the renderer to look up ComputedStyleComponent for per-span properties
    /// (fill, opacity, font-weight, clip-path, mask, filter, baseline-shift, etc.).
    entt::entity sourceEntity = entt::null;

    /// True when this span starts a new text chunk (has explicit x or y positioning).
    /// A new chunk resets the current text position and suppresses cross-span kerning.
    bool startsNewChunk = false;

    /// True when this span's source element has `display:none`. Hidden spans consume
    /// per-character attribute indices (rotate, x, y) but are not rendered and do not
    /// advance the pen position.
    bool hidden = false;

    /// Resolved fill paint for this span (solid color, gradient reference, or none).
    /// Populated by RendererDriver from sourceEntity's ComputedStyleComponent.
    ResolvedPaintServer resolvedFill;

    /// Resolved stroke paint for this span (solid color, gradient reference, or none).
    /// Populated by RendererDriver from sourceEntity's ComputedStyleComponent.
    ResolvedPaintServer resolvedStroke;

    /// CSS `fill-opacity` value for this span (0.0-1.0).
    double fillOpacity = 1.0;

    /// CSS `stroke-opacity` value for this span (0.0-1.0).
    double strokeOpacity = 1.0;

    /// Stroke width in user units for this span.
    double strokeWidth = 0.0;

    /// Stroke line cap used for text outlines in this span.
    StrokeLinecap strokeLinecap = StrokeLinecap::Butt;

    /// Stroke line join used for text outlines in this span.
    StrokeLinejoin strokeLinejoin = StrokeLinejoin::Miter;

    /// Stroke miter limit used for text outlines in this span.
    double strokeMiterLimit = 4.0;

    /// CSS font-weight for this span (100-900, 400=normal, 700=bold).
    /// Populated by RendererDriver from sourceEntity.
    int fontWeight = 400;

    /// CSS font-style for this span (normal/italic/oblique).
    /// Populated by RendererDriver from sourceEntity.
    FontStyle fontStyle = FontStyle::Normal;

    /// CSS font-stretch for this span (condensed/normal/expanded).
    /// Populated by RendererDriver from sourceEntity.
    FontStretch fontStretch = FontStretch::Normal;

    /// CSS font-variant for this span (normal/small-caps).
    /// Populated by RendererDriver from sourceEntity.
    FontVariant fontVariant = FontVariant::Normal;

    /// CSS font-size for this span. When different from the text element's font-size,
    /// the layout engine uses this to shape glyphs at the correct size.
    /// Populated by RendererDriver from sourceEntity.
    Lengthd fontSize;

    /// Indicates whether baseline-shift was set via the `sub` or `super` keywords,
    /// which should be resolved from font OS/2 metrics at layout time.
    enum class BaselineShiftKeyword : uint8_t { Length, Sub, Super };

    /// CSS `baseline-shift` value for this span. For `Length` keyword, this is the explicit
    /// shift value. For `Sub`/`Super`, this is a fallback em-based value; layout engines
    /// should prefer font OS/2 metrics when available.
    Lengthd baselineShift;

    /// Whether baseline-shift was `sub`, `super`, or an explicit length/percentage.
    BaselineShiftKeyword baselineShiftKeyword = BaselineShiftKeyword::Length;

    /// Unresolved baseline-shift values from ancestor tspan elements. Each entry is the ancestor's
    /// (baseline-shift keyword, baseline-shift Lengthd, font-size in pixels). Layout engines
    /// resolve each entry using font OS/2 metrics for sub/super or toPixels() for explicit lengths,
    /// then sum to get the total ancestor shift.
    struct AncestorShift {
      BaselineShiftKeyword keyword;
      Lengthd shift;
      double fontSizePx;
    };
    SmallVector<AncestorShift, 2> ancestorBaselineShifts;

    /// CSS `alignment-baseline` value for this span. When not Auto, overrides the
    /// dominant-baseline for this specific inline element. Populated by RendererDriver from
    /// sourceEntity.
    DominantBaseline alignmentBaseline = DominantBaseline::Auto;

    /// CSS `visibility` value for this span. Hidden/collapsed spans are laid out normally
    /// (they still advance the pen position) but their glyphs are not drawn.
    /// Populated by RendererDriver from sourceEntity.
    Visibility visibility = Visibility::Visible;

    /// CSS `opacity` value for this span (0.0-1.0). Populated by RendererDriver from
    /// sourceEntity.
    double opacity = 1.0;

    /// CSS `letter-spacing` for this span, resolved to pixels. Populated by RendererDriver.
    double letterSpacingPx = 0.0;
    /// CSS `word-spacing` for this span, resolved to pixels. Populated by RendererDriver.
    double wordSpacingPx = 0.0;

    /// Per-span text-decoration bitmask, inherited from the declaring ancestor.
    TextDecoration textDecoration = TextDecoration::None;

    /// Fill paint resolved from the element that declared text-decoration (not this span).
    /// Per CSS Text Decoration §3, decoration uses the declaring element's paint.
    ResolvedPaintServer resolvedDecorationFill;

    /// Stroke paint from the declaring element (for stroking decoration lines).
    ResolvedPaintServer resolvedDecorationStroke;

    /// CSS `fill-opacity` resolved from the text element that provides decoration paint.
    double decorationFillOpacity = 1.0;

    /// CSS `stroke-opacity` resolved from the text element that provides decoration paint.
    double decorationStrokeOpacity = 1.0;

    /// Font size (in pixels) from the declaring element, for computing decoration metrics.
    float decorationFontSizePx = 0.0f;

    /// Stroke width from the declaring element, for stroking decoration lines.
    double decorationStrokeWidth = 0.0;

    /// Number of ancestors that explicitly declared a non-none text-decoration for this span.
    int decorationDeclarationCount = 0;

    /// Per-span text-anchor value. Used for per-chunk text-anchor adjustment:
    /// each text chunk uses the text-anchor of its first rendered character's element.
    /// Populated by RendererDriver from sourceEntity's ComputedStyleComponent.
    TextAnchor textAnchor = TextAnchor::Start;

    /// Per-span textLength override from the source element's TextComponent.
    /// When set, textLength adjustment is applied to this span's run individually
    /// rather than using the global textLength from TextParams.
    std::optional<Lengthd> textLength;
    /// Per-span lengthAdjust from the source element's TextComponent.
    LengthAdjust lengthAdjust = LengthAdjust::Default;

    /// Per-character absolute X positions from \c x attribute lists. Indexed by character
    /// (codepoint) index within the span. \c nullopt means no explicit position — the glyph
    /// advances naturally from the previous character. Index 0 holds the span-start position.
    SmallVector<std::optional<Lengthd>, 1> xList;
    /// Per-character absolute Y positions from \c y attribute lists.
    SmallVector<std::optional<Lengthd>, 1> yList;
    /// Per-character relative X offsets from \c dx attribute lists.
    SmallVector<std::optional<Lengthd>, 1> dxList;
    /// Per-character relative Y offsets from \c dy attribute lists.
    SmallVector<std::optional<Lengthd>, 1> dyList;
    /// Per-character rotation in degrees from \c rotate attribute lists. Per SVG spec, the
    /// last value repeats for all subsequent characters beyond the list length.
    SmallVector<double, 1> rotateList;

    /// Returns true if xList[0] has an explicit value.
    bool hasExplicitX() const { return !xList.empty() && xList[0].has_value(); }
    /// Returns true if yList[0] has an explicit value.
    bool hasExplicitY() const { return !yList.empty() && yList[0].has_value(); }

    /// If set, glyphs in this span are positioned along this path (for \ref xml_textPath).
    std::optional<PathSpline> pathSpline;
    /// The \ref xml_textPath entity that supplied \ref pathSpline, if any.
    entt::entity textPathSourceEntity = entt::null;
    /// Start offset along the path (resolved to pixels).
    double pathStartOffset = 0.0;
    /// True when a \ref xml_textPath element's href could not be resolved. Per SVG spec,
    /// a textPath with an invalid or missing href does not render.
    bool textPathFailed = false;
  };

  // Computed spans with positioning data for rendering.
  SmallVector<TextSpan, 1> spans;
};

}  // namespace donner::svg::components
