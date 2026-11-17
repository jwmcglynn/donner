#include "donner/editor/TextToOutlines.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/FillRule.h"
#include "donner/base/FormatNumber.h"
#include "donner/base/Length.h"
#include "donner/base/Path.h"
#include "donner/base/RcString.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/css/Color.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/core/VectorEffect.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {

namespace {

/// Copy the authored presentation attribute \p name from \p textElement onto
/// \p group when present. Returns true if the attribute was present and copied.
bool copyAttributeIfPresent(const svg::SVGElement& textElement, std::string_view name,
                            svg::SVGElement& group) {
  const std::optional<RcString> value = textElement.getAttribute(xml::XMLQualifiedNameRef(name));
  if (!value.has_value()) {
    return false;
  }
  group.setAttribute(xml::XMLQualifiedNameRef(name), *value);
  return true;
}

/// Serialize a resolved \ref svg::PaintServer to an SVG attribute value, resolving `currentColor`
/// against \p currentColor so the outlined geometry paints identically once detached from the
/// source text (which may carry the `color` that defines `currentColor`). Solid colors serialize
/// to hex; paint-server references serialize back to `url(#id)` (with an optional resolved
/// fallback color).
std::string serializePaint(const svg::PaintServer& paint, const css::RGBA& currentColor) {
  if (paint.is<svg::PaintServer::None>()) {
    return "none";
  }
  if (paint.is<svg::PaintServer::ContextFill>()) {
    return "context-fill";
  }
  if (paint.is<svg::PaintServer::ContextStroke>()) {
    return "context-stroke";
  }
  if (paint.is<svg::PaintServer::Solid>()) {
    return paint.get<svg::PaintServer::Solid>().color.resolve(currentColor, 1.0f).toHexString();
  }
  if (paint.is<svg::PaintServer::ElementReference>()) {
    const auto& ref = paint.get<svg::PaintServer::ElementReference>();
    std::string out = "url(" + std::string(std::string_view(ref.reference.href)) + ")";
    if (ref.fallback.has_value()) {
      out += " " + ref.fallback->resolve(currentColor, 1.0f).toHexString();
    }
    return out;
  }
  return "none";
}

/// The paint-affecting computed style of one element, serialized to SVG attribute strings. Resolved
/// from \ref svg::SVGElement::getComputedStyle so it captures every styling form (presentation
/// attributes, CSS rules, inline `style`, inheritance, `currentColor`, and `url(#)` references),
/// not just presentation attributes.
struct ResolvedPaint {
  std::string fill;              ///< `fill` value (`none`, hex, or `url(#id)`).
  std::string fillRule;          ///< `fill-rule` value (`nonzero` / `evenodd`).
  std::string fillOpacity;       ///< `fill-opacity` value, with any run opacity folded in.
  std::string stroke;            ///< `stroke` value (`none`, hex, or `url(#id)`).
  std::string strokeWidth;       ///< `stroke-width` value.
  std::string strokeOpacity;     ///< `stroke-opacity` value, with any run opacity folded in.
  std::string strokeDasharray;   ///< `stroke-dasharray` value (`none` or lengths).
  std::string strokeDashoffset;  ///< `stroke-dashoffset` value.
  std::string strokeLinecap;     ///< `stroke-linecap` value.
  std::string strokeLinejoin;    ///< `stroke-linejoin` value.
  std::string strokeMiterlimit;  ///< `stroke-miterlimit` value.

  /// Whether the stroke has a width the renderer strokes; a zero width paints nothing.
  bool strokeHasWidth = true;

  /// Whether the stroke is painted at all, as the renderer decides: a stroke paint and a positive
  /// width.
  bool hasStroke() const { return stroke != "none" && strokeHasWidth; }
};

/// Computed `opacity` of \p element alone. `opacity` is not an inherited property, so this is the
/// element's own value, independent of its ancestors.
double ownOpacity(const svg::SVGElement& element) {
  return element.getComputedStyle().opacity.getOr(1.0);
}

/// Effective `opacity` of the glyph run painted by \p source, relative to \p textRoot: the product
/// of the own `opacity` of \p source and of every element between it and \p textRoot.
///
/// \p textRoot is excluded because its `opacity` rides on the outline group that replaces it;
/// including it here would composite the root's opacity a second time on every run.
double runOpacityBelowRoot(const svg::SVGElement& source, const svg::SVGElement& textRoot) {
  double opacity = 1.0;
  std::optional<svg::SVGElement> current = source;
  while (current.has_value()) {
    if (*current == textRoot) {
      return opacity;
    }
    opacity *= ownOpacity(*current);
    current = current->parentElement();
  }

  // Every glyph of a converted `<text>` is painted by that element or a descendant of it, so the
  // walk above always reaches the text root. A source from outside that subtree contributes no
  // opacity rather than a product over an unrelated ancestor chain.
  assert(false && "glyph source is outside the converted text subtree");
  return 1.0;
}

/// Serializes an opacity product, rounded to six decimal places so a product such as 0.8 * 0.4
/// writes as `0.32`, not `0.32000000000000006`. The rounding moves the value by at most 5e-7, far
/// below one step of 8-bit paint alpha.
std::string formatOpacity(double opacity) {
  return detail::FormatNumberForSVG(std::round(opacity * 1e6) / 1e6);
}

/// Serializes a stroke length (`stroke-width`, a dash length or `stroke-dashoffset`) of an element
/// whose font size is \p fontSizePx. A font-relative
/// length (`em`, `ex`, `ch`, `rem`) is written in user units, resolved against that font size as
/// the renderer resolves a text span's stroke lengths, because a converted path inherits its font
/// size from the outline group instead. Every other unit resolves the same on the path and is
/// written as authored.
std::string serializeStrokeLength(const Lengthd& length, double fontSizePx) {
  using Unit = Lengthd::Unit;
  if (length.unit != Unit::Em && length.unit != Unit::Ex && length.unit != Unit::Ch &&
      length.unit != Unit::Rem) {
    return std::string(std::string_view(length.toRcString()));
  }
  return detail::FormatNumberForSVG(length.toPixels(
      Box2d(), FontMetrics::DefaultsWithFontSize(fontSizePx), Lengthd::Extent::Mixed));
}

/// Serializes a computed `stroke-dasharray` (\ref serializeStrokeLength for each length).
std::string serializeDasharray(const std::optional<svg::StrokeDasharray>& dashes,
                               double fontSizePx) {
  if (!dashes.has_value() || dashes->empty()) {
    return "none";
  }
  std::string out;
  for (const Lengthd& dash : *dashes) {
    out += out.empty() ? "" : " ";
    out += serializeStrokeLength(dash, fontSizePx);
  }
  return out;
}

/// Serializes a value through its CSS `operator<<`.
template <typename T>
std::string serializeKeyword(const T& value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

/// Resolve the paint-affecting computed style of \p element into serialized SVG attribute strings.
///
/// \p runOpacity is folded into `fill-opacity` and `stroke-opacity`, as the text renderer folds a
/// span's opacity into the alpha of each glyph's fill and each chunk's stroke rather than
/// compositing the span as a layer. A path that paints only a fill or only a stroke then renders
/// exactly as that paint did on the `<text>`; an `opacity` on the path would instead composite it
/// as a layer, which rounds differently.
ResolvedPaint resolvePaint(const svg::SVGElement& element, double runOpacity) {
  static constexpr css::RGBA kBlack = css::RGBA(0, 0, 0, 0xFF);
  const svg::PropertyRegistry& style = element.getComputedStyle();

  const css::RGBA currentColor = style.color.getOr(css::Color(kBlack)).resolve(kBlack, 1.0f);

  ResolvedPaint paint;
  paint.fill = serializePaint(
      style.fill.getOr(svg::PaintServer(svg::PaintServer::Solid(css::Color(kBlack)))),
      currentColor);
  paint.fillRule =
      style.fillRule.getOr(FillRule::NonZero) == FillRule::EvenOdd ? "evenodd" : "nonzero";
  paint.fillOpacity = formatOpacity(style.fillOpacity.getOr(1.0) * runOpacity);
  paint.stroke =
      serializePaint(style.stroke.getOr(svg::PaintServer(svg::PaintServer::None())), currentColor);
  const double fontSizePx = style.fontSize.getOr(Lengthd(16, Lengthd::Unit::Px))
                                .toPixels(Box2d(), FontMetrics::DefaultsWithFontSize(12.0));
  const Lengthd strokeWidth = style.strokeWidth.getOr(Lengthd(1, Lengthd::Unit::None));
  paint.strokeWidth = serializeStrokeLength(strokeWidth, fontSizePx);
  paint.strokeHasWidth = strokeWidth.value > 0.0;
  paint.strokeOpacity = formatOpacity(style.strokeOpacity.getOr(1.0) * runOpacity);
  paint.strokeDasharray = serializeDasharray(style.strokeDasharray.get(), fontSizePx);
  paint.strokeDashoffset = serializeStrokeLength(
      style.strokeDashoffset.getOr(Lengthd(0, Lengthd::Unit::None)), fontSizePx);
  paint.strokeLinecap = serializeKeyword(style.strokeLinecap.getOr(svg::StrokeLinecap::Butt));
  paint.strokeLinejoin = serializeKeyword(style.strokeLinejoin.getOr(svg::StrokeLinejoin::Miter));
  paint.strokeMiterlimit = detail::FormatNumberForSVG(style.strokeMiterlimit.getOr(4.0));
  return paint;
}

std::optional<std::string> preflightOutlineTarget(svg::SVGDocument& document,
                                                  const svg::SVGElement& textElement) {
  const auto preflight = document.preflightFontResourcesForElement(textElement);
  using Status = svg::FontResourcePreflight::Status;
  switch (preflight.status) {
    case Status::Ready: break;
    case Status::InvalidTarget:
      return "Convert to outlines failed: target is not attached to this document.";
    case Status::ResourceLimit:
      return "Convert to outlines failed: the font resource limit was exceeded.";
    case Status::NeedsRender:
      return "Convert to outlines is waiting for render preparation. Try again after the next "
             "frame.";
    default:
      return "The selected text's font is not ready. Load or retry the font, then try again.";
  }
  if (textElement.tagName().name != svg::SVGTextElement::Tag) {
    return "Convert to outlines failed: selection is not a <text> element.";
  }
  return std::nullopt;
}

std::optional<std::string> validateOutlineGlyphs(svg::SVGDocument& document,
                                                 const svg::SVGElement& textElement,
                                                 std::span<const svg::TextGlyphOutline> glyphs) {
  if (document.fontResourcesExceeded()) {
    return "Convert to outlines failed: the font resource limit was exceeded.";
  }

  for (const auto& face : document.fontDependenciesForElement(textElement)) {
    if (face.state != svg::FontFaceLoadState::Loaded) {
      return "The selected text's font is not ready. Load or retry the font, then try again.";
    }
  }

  // Empty outlines (missing font, layout failure, empty text) fail the whole conversion without
  // mutating.
  if (glyphs.empty()) {
    return "Convert to outlines failed: <text> produced no glyph outlines.";
  }
  bool anyNonEmpty = false;
  for (const svg::TextGlyphOutline& glyph : glyphs) {
    if (!glyph.path.empty()) {
      anyNonEmpty = true;
      break;
    }
  }
  if (!anyNonEmpty) {
    return "Convert to outlines failed: <text> produced only empty glyph outlines.";
  }

  return std::nullopt;
}

/// Which paints a `<path>` of the outlines paints.
enum class PathPaints : std::uint8_t {
  Fill,    ///< One glyph, painting only its fill.
  Stroke,  ///< One text chunk of a stroked run, painting only its stroke.
};

/// A paint attribute the outline group pins and a path overrides where its run differs.
struct PinnedPaintAttribute {
  std::string_view name;               ///< Attribute name.
  std::string ResolvedPaint::* value;  ///< The serialized value in \ref ResolvedPaint.
  PathPaints paints;                   ///< The paths that paint it.
};

/// Every paint attribute the converter carries. The group writes all of them, so none is
/// inherited from the `<text>`'s ancestors, and each path writes those of the paint it paints
/// where its run's value differs from the group's.
constexpr std::array<PinnedPaintAttribute, 11> kPinnedPaintAttributes = {{
    {"fill", &ResolvedPaint::fill, PathPaints::Fill},
    {"fill-rule", &ResolvedPaint::fillRule, PathPaints::Fill},
    {"fill-opacity", &ResolvedPaint::fillOpacity, PathPaints::Fill},
    {"stroke", &ResolvedPaint::stroke, PathPaints::Stroke},
    {"stroke-width", &ResolvedPaint::strokeWidth, PathPaints::Stroke},
    {"stroke-opacity", &ResolvedPaint::strokeOpacity, PathPaints::Stroke},
    {"stroke-dasharray", &ResolvedPaint::strokeDasharray, PathPaints::Stroke},
    {"stroke-dashoffset", &ResolvedPaint::strokeDashoffset, PathPaints::Stroke},
    {"stroke-linecap", &ResolvedPaint::strokeLinecap, PathPaints::Stroke},
    {"stroke-linejoin", &ResolvedPaint::strokeLinejoin, PathPaints::Stroke},
    {"stroke-miterlimit", &ResolvedPaint::strokeMiterlimit, PathPaints::Stroke},
}};

void applyPaintOverrides(svg::SVGElement& pathElement, const ResolvedPaint& runPaint,
                         const ResolvedPaint& groupPaint, PathPaints paints) {
  for (const PinnedPaintAttribute& attribute : kPinnedPaintAttributes) {
    if (attribute.paints == paints && runPaint.*attribute.value != groupPaint.*attribute.value) {
      pathElement.setAttribute(attribute.name, runPaint.*attribute.value);
    }
  }
}

/// The paint of the outline group: the converted `<text>`'s own computed paint, with a stroke the
/// renderer does not paint (a zero width) written as `stroke="none"`, so the paths compare their
/// runs' strokes against what the group actually paints.
ResolvedPaint resolveGroupPaint(const svg::SVGElement& textElement) {
  ResolvedPaint paint = resolvePaint(textElement, 1.0);
  if (!paint.hasStroke()) {
    paint.stroke = "none";
  }
  return paint;
}

/// Writes \p groupPaint onto the outline group, with \p opacity, the converted `<text>`'s own
/// `opacity`. Unlike a span's, the text's opacity stays a group property: it composites the whole
/// outline set as one layer, as it did on the `<text>`.
///
/// Every paint attribute \ref applyPaintOverrides compares against (\ref kPinnedPaintAttributes) is
/// written, defaults included:
/// the group takes the `<text>`'s place under the same parent, so an attribute left off would be
/// inherited from the `<text>`'s ancestors instead of from its computed style.
void applyGroupPaint(svg::SVGElement& groupElement, const ResolvedPaint& groupPaint,
                     double opacity) {
  for (const PinnedPaintAttribute& attribute : kPinnedPaintAttributes) {
    groupElement.setAttribute(attribute.name, groupPaint.*attribute.value);
  }
  if (opacity != 1.0) {
    groupElement.setAttribute("opacity", detail::FormatNumberForSVG(opacity));
  }
}

/**
 * Builds the detached outline `<path>`s of one converted `<text>`, painting each text run as the
 * renderer paints it: the run's fill glyph by glyph and its stroke one text chunk at a time, in
 * the run's `paint-order`.
 *
 * A run without a visible stroke becomes one fill-only path per glyph. A stroked run becomes one
 * fill-only path per glyph, none when its fill is `none`, and one stroke-only path per text chunk,
 * holding every glyph of the chunk, so overlapping glyph strokes are covered once, as they are on
 * the `<text>`. Empty glyph paths (e.g. whitespace) contribute no geometry and are skipped. Each
 * path overrides the group paint only where its source run's computed paint differs, writing only
 * the paint it paints, so per-<tspan> fill/stroke is preserved without collapsing to the group
 * level. The opacity of the spans between a run and the `<text>` is folded into the paint's
 * `fill-opacity` or `stroke-opacity`, as the renderer folds it into each glyph's fill alpha and
 * each chunk's stroke alpha (see \ref resolvePaint).
 */
class OutlineRunBuilder {
public:
  /// @param document Document the detached elements are created in.
  /// @param textElement The converted `<text>`.
  /// @param groupId Id of the outline group; path ids are numbered after it.
  /// @param groupPaint Paint the outline group carries.
  /// @param out Receives the paths in paint order.
  OutlineRunBuilder(svg::SVGDocument& document, const svg::SVGElement& textElement,
                    std::string groupId, const ResolvedPaint& groupPaint,
                    std::vector<svg::SVGElement>& out)
      : document_(document),
        textElement_(textElement),
        groupId_(std::move(groupId)),
        groupPaint_(groupPaint),
        out_(out),
        nonScalingStroke_(textElement.getComputedStyle().vectorEffect.getOr(
                              svg::VectorEffect::None) == svg::VectorEffect::NonScalingStroke) {}

  /// Adds the paths of one text run, whose glyphs share one source element.
  void addRun(std::span<const svg::TextGlyphOutline> run) {
    const svg::SVGElement& source = run.front().source;
    const bool ownPaint = source != textElement_;
    const ResolvedPaint paint =
        ownPaint ? resolvePaint(source, runOpacityBelowRoot(source, textElement_)) : groupPaint_;
    if (!paint.hasStroke()) {
      addGlyphs(run, ownPaint, paint);
      return;
    }
    const bool paintsFill = paint.fill != "none";
    if (source.getComputedStyle().paintOrder.getOr(svg::PaintOrder()).strokeBeforeFill()) {
      addChunkStrokes(run, ownPaint, paint);
      if (paintsFill) {
        addGlyphs(run, ownPaint, paint);
      }
    } else {
      if (paintsFill) {
        addGlyphs(run, ownPaint, paint);
      }
      addChunkStrokes(run, ownPaint, paint);
    }
  }

private:
  /// One fill-only path per non-empty glyph of \p run.
  void addGlyphs(std::span<const svg::TextGlyphOutline> run, bool ownPaint,
                 const ResolvedPaint& paint) {
    for (const svg::TextGlyphOutline& glyph : run) {
      if (!glyph.path.empty()) {
        addPath(glyph.path.toSVGPathData(), ownPaint, paint, PathPaints::Fill);
      }
    }
  }

  /// One stroke-only path per text chunk of \p run, holding every glyph of the chunk.
  void addChunkStrokes(std::span<const svg::TextGlyphOutline> run, bool ownPaint,
                       const ResolvedPaint& paint) {
    std::string chunkData;
    for (const svg::TextGlyphOutline& glyph : run) {
      if (glyph.path.empty()) {
        continue;
      }
      if (glyph.startsTextChunk && !chunkData.empty()) {
        addPath(chunkData, ownPaint, paint, PathPaints::Stroke);
        chunkData.clear();
      }
      chunkData += chunkData.empty() ? "" : " ";
      chunkData += std::string_view(glyph.path.toSVGPathData());
    }
    if (!chunkData.empty()) {
      addPath(chunkData, ownPaint, paint, PathPaints::Stroke);
    }
  }

  void addPath(std::string_view pathData, bool ownPaint, const ResolvedPaint& paint,
               PathPaints paints) {
    svg::SVGPathElement pathElement = svg::SVGPathElement::Create(document_);
    pathElement.setAttribute("id", groupId_ + "_" + std::to_string(nextIndex_++));
    pathElement.setAttribute("d", pathData);
    if (ownPaint) {
      applyPaintOverrides(pathElement, paint, groupPaint_, paints);
    }
    // Turn off the paint the path does not paint, where it would otherwise inherit the group's.
    if (paints == PathPaints::Fill && groupPaint_.hasStroke()) {
      pathElement.setAttribute("stroke", "none");
    } else if (paints == PathPaints::Stroke && groupPaint_.fill != "none") {
      pathElement.setAttribute("fill", "none");
    }
    // The text's `vector-effect` applies to every run's stroke, and it is not inherited, so each
    // stroke path carries it.
    if (paints == PathPaints::Stroke && nonScalingStroke_) {
      pathElement.setAttribute("vector-effect", "non-scaling-stroke");
    }
    out_.push_back(pathElement);
  }

  svg::SVGDocument& document_;
  const svg::SVGElement& textElement_;
  std::string groupId_;
  const ResolvedPaint& groupPaint_;
  std::vector<svg::SVGElement>& out_;
  int nextIndex_ = 0;
  /// Whether the converted `<text>` has `vector-effect: non-scaling-stroke`.
  bool nonScalingStroke_ = false;
};

ConvertTextToOutlinesResult buildDetachedTextOutlines(svg::SVGDocument& document,
                                                      const svg::SVGElement& textElement) {
  ConvertTextToOutlinesResult result;

  // Resolve placed glyph outlines via the renderer-facing text geometry, retaining the source
  // element that painted each glyph. This routes through `TextEngine::computedGlyphOutlines()` -
  // the SAME positioned outlines the renderer fills - so the converted paths land exactly where
  // the text rendered, and each glyph carries the `<text>`/`<tspan>` whose computed style colors
  // it (so per-span paint survives). `convertToOutlineGlyphs()` is non-const, so operate on a copy
  // of the value-type element handle.
  svg::SVGTextElement text = textElement.cast<svg::SVGTextElement>();
  const std::vector<svg::TextGlyphOutline> glyphs = text.convertToOutlineGlyphs();

  if (auto error = validateOutlineGlyphs(document, textElement, glyphs)) {
    result.error = std::move(*error);
    return result;
  }

  // Group id: base it on the text element's id when present, else a stable name.
  const RcString textId = textElement.id();
  const std::string groupId =
      textId.empty() ? std::string("text_outlines") : (textId.str() + "_outlines");
  result.outlineGroupId = groupId;

  // Build the detached replacement group. Paint is resolved from the text element's computed style
  // (not just its presentation attributes) so CSS, inline styles, inheritance, currentColor, and
  // paint-server references are all preserved. Per-<tspan> paint is applied on the individual
  // glyph paths below, overriding the group only where a run's paint differs.
  svg::SVGGElement group = svg::SVGGElement::Create(document);
  group.setAttribute("id", groupId);
  group.setAttribute("data-donner-converted-from", "text");
  svg::SVGElement groupElement = group;

  // The text element's own `opacity` composites the whole run set as one layer, exactly as it did
  // on the `<text>`; `transform` positions the outlined geometry. Carry both onto the group so it
  // sits and composites where the text did.
  const ResolvedPaint groupPaint = resolveGroupPaint(textElement);
  applyGroupPaint(groupElement, groupPaint, ownOpacity(textElement));
  copyAttributeIfPresent(textElement, "transform", groupElement);

  // Build the detached outline paths run by run (see \ref OutlineRunBuilder). Each text run holds
  // consecutive glyphs.
  OutlineRunBuilder builder(document, textElement, groupId, groupPaint, result.outlinePaths);
  for (std::size_t begin = 0; begin < glyphs.size();) {
    std::size_t end = begin + 1;
    while (end < glyphs.size() && glyphs[end].run == glyphs[begin].run) {
      ++end;
    }
    builder.addRun(std::span<const svg::TextGlyphOutline>(glyphs).subspan(begin, end - begin));
    begin = end;
  }

  result.ok = true;
  result.outlineGroup = groupElement;
  return result;
}

}  // namespace

ConvertTextsToOutlinesResult convertTextsToOutlines(svg::SVGDocument& document,
                                                    std::span<const svg::SVGElement> textElements) {
  [[maybe_unused]] const auto access = document.writeAccess();
  ConvertTextsToOutlinesResult batch;
  // Detached construction shares storage with the live tree and may mark it dirty.
  for (const auto& element : textElements) {
    if (auto error = preflightOutlineTarget(document, element)) {
      batch.error = std::move(*error);
      return batch;
    }
  }
  batch.conversions.reserve(textElements.size());
  for (const auto& element : textElements) {
    auto result = buildDetachedTextOutlines(document, element);
    if (!result.ok) {
      batch.error = std::move(result.error);
      batch.conversions.clear();
      return batch;
    }
    batch.conversions.push_back(std::move(result));
  }
  batch.ok = true;
  return batch;
}

ConvertTextToOutlinesResult convertTextToOutlines(svg::SVGDocument& document,
                                                  const svg::SVGElement& textElement) {
  auto batch = convertTextsToOutlines(document, std::span(&textElement, 1));
  if (!batch.ok) {
    return {.error = std::move(batch.error)};
  }
  return std::move(batch.conversions.front());
}

}  // namespace donner::editor
