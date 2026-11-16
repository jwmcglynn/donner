#include "donner/editor/TextToOutlines.h"

#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/FillRule.h"
#include "donner/base/FormatNumber.h"
#include "donner/base/Length.h"
#include "donner/base/Path.h"
#include "donner/base/RcString.h"
#include "donner/css/Color.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGTextElement.h"
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
  std::string fill;           ///< `fill` value (`none`, hex, or `url(#id)`).
  std::string fillRule;       ///< `fill-rule` value (`nonzero` / `evenodd`).
  std::string fillOpacity;    ///< `fill-opacity` value.
  std::string stroke;         ///< `stroke` value (`none`, hex, or `url(#id)`).
  std::string strokeWidth;    ///< `stroke-width` value.
  std::string strokeOpacity;  ///< `stroke-opacity` value.
  /// `opacity` value to write onto the element this paint is serialized to. Unlike the paint
  /// properties, `opacity` does not inherit and does not override an enclosing value - every
  /// `opacity` between an element and the canvas multiplies - so this holds only the share of the
  /// opacity the receiving element is responsible for.
  std::string opacity;

  bool hasStroke() const { return stroke != "none"; }
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

/// Resolve the paint-affecting computed style of \p element into serialized SVG attribute strings,
/// carrying \p opacity as the receiving element's own `opacity`.
ResolvedPaint resolvePaint(const svg::SVGElement& element, double opacity) {
  static constexpr css::RGBA kBlack = css::RGBA(0, 0, 0, 0xFF);
  const svg::PropertyRegistry& style = element.getComputedStyle();

  const css::RGBA currentColor = style.color.getOr(css::Color(kBlack)).resolve(kBlack, 1.0f);

  ResolvedPaint paint;
  paint.fill = serializePaint(
      style.fill.getOr(svg::PaintServer(svg::PaintServer::Solid(css::Color(kBlack)))),
      currentColor);
  paint.fillRule =
      style.fillRule.getOr(FillRule::NonZero) == FillRule::EvenOdd ? "evenodd" : "nonzero";
  paint.fillOpacity = detail::FormatNumberForSVG(style.fillOpacity.getOr(1.0));
  paint.stroke =
      serializePaint(style.stroke.getOr(svg::PaintServer(svg::PaintServer::None())), currentColor);
  paint.strokeWidth = std::string(
      std::string_view(style.strokeWidth.getOr(Lengthd(1, Lengthd::Unit::None)).toRcString()));
  paint.strokeOpacity = detail::FormatNumberForSVG(style.strokeOpacity.getOr(1.0));
  paint.opacity = detail::FormatNumberForSVG(opacity);
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

void applyPaintOverrides(svg::SVGElement& pathElement, const ResolvedPaint& runPaint,
                         const ResolvedPaint& groupPaint) {
  if (runPaint.fill != groupPaint.fill) {
    pathElement.setAttribute("fill", runPaint.fill);
  }
  if (runPaint.fillRule != groupPaint.fillRule) {
    pathElement.setAttribute("fill-rule", runPaint.fillRule);
  }
  if (runPaint.fillOpacity != groupPaint.fillOpacity) {
    pathElement.setAttribute("fill-opacity", runPaint.fillOpacity);
  }
  if (runPaint.stroke != groupPaint.stroke) {
    pathElement.setAttribute("stroke", runPaint.stroke);
  }
  if (runPaint.hasStroke()) {
    if (runPaint.strokeWidth != groupPaint.strokeWidth) {
      pathElement.setAttribute("stroke-width", runPaint.strokeWidth);
    }
    if (runPaint.strokeOpacity != groupPaint.strokeOpacity) {
      pathElement.setAttribute("stroke-opacity", runPaint.strokeOpacity);
    }
  }
  // The run's opacity multiplies with the group's rather than replacing it, so it is written
  // whenever the run is not fully opaque - never compared against the group's value.
  if (runPaint.opacity != "1") {
    pathElement.setAttribute("opacity", runPaint.opacity);
  }
}

void applyGroupPaint(svg::SVGElement& groupElement, const ResolvedPaint& groupPaint) {
  groupElement.setAttribute("fill", groupPaint.fill);
  if (groupPaint.fillRule != "nonzero") {
    groupElement.setAttribute("fill-rule", groupPaint.fillRule);
  }
  if (groupPaint.fillOpacity != "1") {
    groupElement.setAttribute("fill-opacity", groupPaint.fillOpacity);
  }
  if (groupPaint.hasStroke()) {
    groupElement.setAttribute("stroke", groupPaint.stroke);
    groupElement.setAttribute("stroke-width", groupPaint.strokeWidth);
    if (groupPaint.strokeOpacity != "1") {
      groupElement.setAttribute("stroke-opacity", groupPaint.strokeOpacity);
    }
  }
  if (groupPaint.opacity != "1") {
    groupElement.setAttribute("opacity", groupPaint.opacity);
  }
}

/**
 * Builds the detached outline elements of one converted `<text>`, painting each layout run as the
 * renderer paints it: the run's fill glyph by glyph and its stroke one text chunk at a time, in
 * the run's `paint-order`.
 *
 * A run without a stroke becomes one path per glyph. A stroked run becomes one fill-only path per
 * glyph and one stroke-only path per text chunk, holding every glyph of the chunk, so overlapping
 * glyph strokes are covered once, as they are on the `<text>`. Empty glyph paths (e.g.
 * whitespace) contribute no geometry and are skipped. Each path overrides the group paint only
 * where its source run's computed paint differs, so per-<tspan> fill/stroke is preserved without
 * collapsing to the group level, and carries the `opacity` of the spans between it and the
 * `<text>`, which multiplies with the group's. A stroked run with such an opacity puts its paths
 * in a `<g>` that carries it, so the run's fill and stroke composite as one layer, as a single
 * path's fill and stroke do.
 */
class OutlineRunBuilder {
public:
  /// @param document Document the detached elements are created in.
  /// @param textElement The converted `<text>`.
  /// @param groupId Id of the outline group; element ids are numbered after it.
  /// @param groupPaint Paint the outline group carries.
  /// @param out Receives the top-level elements in paint order.
  OutlineRunBuilder(svg::SVGDocument& document, const svg::SVGElement& textElement,
                    std::string groupId, const ResolvedPaint& groupPaint,
                    std::vector<svg::SVGElement>& out)
      : document_(document),
        textElement_(textElement),
        groupId_(std::move(groupId)),
        groupPaint_(groupPaint),
        out_(out) {}

  /// Adds the elements of one layout run, whose glyphs share one source element.
  void addRun(std::span<const svg::TextGlyphOutline> run) {
    const svg::SVGElement& source = run.front().source;
    const bool ownPaint = source != textElement_;
    ResolvedPaint paint =
        ownPaint ? resolvePaint(source, runOpacityBelowRoot(source, textElement_)) : groupPaint_;
    if (!paint.hasStroke()) {
      addFills(run, ownPaint, paint, std::nullopt);
      return;
    }

    // The run's opacity moves to a wrapper, so its paths carry none of their own.
    if (ownPaint && paint.opacity != "1") {
      svg::SVGGElement wrapper = svg::SVGGElement::Create(document_);
      wrapper.setAttribute("id", nextId());
      wrapper.setAttribute("opacity", paint.opacity);
      out_.push_back(wrapper);
      wrapper_ = out_.back();
      paint.opacity = "1";
    }
    if (source.getComputedStyle().paintOrder.getOr(svg::PaintOrder()).strokeBeforeFill()) {
      addChunkStrokes(run, ownPaint, paint);
      addFills(run, ownPaint, paint, "stroke");
    } else {
      addFills(run, ownPaint, paint, "stroke");
      addChunkStrokes(run, ownPaint, paint);
    }
    wrapper_.reset();
  }

private:
  /// One path per non-empty glyph, with \p noPaint, if any, set to `none`.
  void addFills(std::span<const svg::TextGlyphOutline> run, bool ownPaint,
                const ResolvedPaint& paint, std::optional<std::string_view> noPaint) {
    for (const svg::TextGlyphOutline& glyph : run) {
      if (!glyph.path.empty()) {
        addPath(glyph.path.toSVGPathData(), ownPaint, paint, noPaint);
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
        addPath(chunkData, ownPaint, paint, "fill");
        chunkData.clear();
      }
      chunkData += chunkData.empty() ? "" : " ";
      chunkData += std::string_view(glyph.path.toSVGPathData());
    }
    if (!chunkData.empty()) {
      addPath(chunkData, ownPaint, paint, "fill");
    }
  }

  void addPath(std::string_view pathData, bool ownPaint, const ResolvedPaint& paint,
               std::optional<std::string_view> noPaint) {
    svg::SVGPathElement pathElement = svg::SVGPathElement::Create(document_);
    pathElement.setAttribute("id", nextId());
    pathElement.setAttribute("d", pathData);
    if (ownPaint) {
      applyPaintOverrides(pathElement, paint, groupPaint_);
    }
    if (noPaint.has_value()) {
      pathElement.setAttribute(xml::XMLQualifiedNameRef(*noPaint), "none");
    }
    if (wrapper_.has_value()) {
      wrapper_->appendChild(pathElement);
    } else {
      out_.push_back(pathElement);
    }
  }

  std::string nextId() { return groupId_ + "_" + std::to_string(nextIndex_++); }

  svg::SVGDocument& document_;
  const svg::SVGElement& textElement_;
  std::string groupId_;
  const ResolvedPaint& groupPaint_;
  std::vector<svg::SVGElement>& out_;
  std::optional<svg::SVGElement> wrapper_;  ///< Wrapper of the run being added, if it has one.
  int nextIndex_ = 0;
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
  const ResolvedPaint groupPaint = resolvePaint(textElement, ownOpacity(textElement));
  applyGroupPaint(groupElement, groupPaint);
  copyAttributeIfPresent(textElement, "transform", groupElement);

  // Build the detached outline elements run by run (see \ref OutlineRunBuilder). Each layout run
  // holds consecutive glyphs.
  OutlineRunBuilder builder(document, textElement, groupId, groupPaint, result.outlinePaths);
  for (std::size_t begin = 0; begin < glyphs.size();) {
    std::size_t end = begin + 1;
    while (end < glyphs.size() && glyphs[end].run == glyphs[begin].run) {
      ++end;
    }
    builder.addRun(std::span<const svg::TextGlyphOutline>(glyphs.data() + begin, end - begin));
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
