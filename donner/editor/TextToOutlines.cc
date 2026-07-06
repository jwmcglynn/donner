#include "donner/editor/TextToOutlines.h"

#include <optional>
#include <string>
#include <string_view>
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

  bool hasStroke() const { return stroke != "none"; }
};

/// Resolve the paint-affecting computed style of \p element into serialized SVG attribute strings.
ResolvedPaint resolvePaint(const svg::SVGElement& element) {
  static constexpr css::RGBA kBlack = css::RGBA(0, 0, 0, 0xFF);
  const svg::PropertyRegistry& style = element.getComputedStyle();

  const css::RGBA currentColor = style.color.getOr(css::Color(kBlack)).resolve(kBlack, 1.0f);

  ResolvedPaint paint;
  paint.fill = serializePaint(
      style.fill.getOr(svg::PaintServer(svg::PaintServer::Solid(css::Color(kBlack)))), currentColor);
  paint.fillRule = style.fillRule.getOr(FillRule::NonZero) == FillRule::EvenOdd ? "evenodd"
                                                                                : "nonzero";
  paint.fillOpacity = detail::FormatNumberForSVG(style.fillOpacity.getOr(1.0));
  paint.stroke =
      serializePaint(style.stroke.getOr(svg::PaintServer(svg::PaintServer::None())), currentColor);
  paint.strokeWidth =
      std::string(std::string_view(style.strokeWidth.getOr(Lengthd(1, Lengthd::Unit::None))
                                       .toRcString()));
  paint.strokeOpacity = detail::FormatNumberForSVG(style.strokeOpacity.getOr(1.0));
  return paint;
}

}  // namespace

ConvertTextToOutlinesResult convertTextToOutlines(svg::SVGDocument& document,
                                                  const svg::SVGElement& textElement) {
  ConvertTextToOutlinesResult result;

  // The conversion only applies to `<text>` elements.
  if (textElement.tagName().name != svg::SVGTextElement::Tag) {
    result.error = "Convert to outlines failed: selection is not a <text> element.";
    return result;
  }

  // Resolve placed glyph outlines via the renderer-facing text geometry, retaining the source
  // element that painted each glyph. This routes through `TextEngine::computedGlyphOutlines()` -
  // the SAME positioned outlines the renderer fills - so the converted paths land exactly where
  // the text rendered, and each glyph carries the `<text>`/`<tspan>` whose computed style colors
  // it (so per-span paint survives). `convertToOutlineGlyphs()` is non-const, so operate on a copy
  // of the value-type element handle.
  svg::SVGTextElement text = textElement.cast<svg::SVGTextElement>();
  const std::vector<svg::TextGlyphOutline> glyphs = text.convertToOutlineGlyphs();

  // Empty outlines (missing font, layout failure, empty text) fail the whole conversion without
  // mutating.
  if (glyphs.empty()) {
    result.error = "Convert to outlines failed: <text> produced no glyph outlines.";
    return result;
  }
  bool anyNonEmpty = false;
  for (const svg::TextGlyphOutline& glyph : glyphs) {
    if (!glyph.path.empty()) {
      anyNonEmpty = true;
      break;
    }
  }
  if (!anyNonEmpty) {
    result.error = "Convert to outlines failed: <text> produced only empty glyph outlines.";
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

  const ResolvedPaint groupPaint = resolvePaint(textElement);
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
  // `opacity` is a (non-inherited) group property, and `transform` positions the outlined geometry;
  // carry both from the text element so the group sits and composites exactly as the text did.
  const std::string opacity =
      detail::FormatNumberForSVG(textElement.getComputedStyle().opacity.getOr(1.0));
  if (opacity != "1") {
    groupElement.setAttribute("opacity", opacity);
  }
  copyAttributeIfPresent(textElement, "transform", groupElement);

  // Build one detached `<path>` per placed glyph, in paint order. Empty glyph paths (e.g.
  // whitespace) are skipped - they contribute no geometry. Each path overrides the group paint
  // only where its source run's computed paint differs, so per-<tspan> fill/stroke is preserved
  // without collapsing to the group level. Resolution is cached per contiguous same-source run.
  int pathIndex = 0;
  std::optional<svg::SVGElement> cachedSource;
  ResolvedPaint cachedPaint;
  for (const svg::TextGlyphOutline& glyph : glyphs) {
    if (glyph.path.empty()) {
      continue;
    }
    svg::SVGPathElement pathElement = svg::SVGPathElement::Create(document);
    pathElement.setAttribute("id", groupId + "_" + std::to_string(pathIndex));
    pathElement.setAttribute("d", std::string_view(glyph.path.toSVGPathData()));

    if (glyph.source != textElement) {
      if (!cachedSource.has_value() || *cachedSource != glyph.source) {
        cachedSource = glyph.source;
        cachedPaint = resolvePaint(glyph.source);
      }
      const ResolvedPaint& runPaint = cachedPaint;

      svg::SVGElement pathAsElement = pathElement;
      if (runPaint.fill != groupPaint.fill) {
        pathAsElement.setAttribute("fill", runPaint.fill);
      }
      if (runPaint.fillRule != groupPaint.fillRule) {
        pathAsElement.setAttribute("fill-rule", runPaint.fillRule);
      }
      if (runPaint.fillOpacity != groupPaint.fillOpacity) {
        pathAsElement.setAttribute("fill-opacity", runPaint.fillOpacity);
      }
      if (runPaint.stroke != groupPaint.stroke) {
        pathAsElement.setAttribute("stroke", runPaint.stroke);
      }
      if (runPaint.hasStroke()) {
        if (runPaint.strokeWidth != groupPaint.strokeWidth) {
          pathAsElement.setAttribute("stroke-width", runPaint.strokeWidth);
        }
        if (runPaint.strokeOpacity != groupPaint.strokeOpacity) {
          pathAsElement.setAttribute("stroke-opacity", runPaint.strokeOpacity);
        }
      }
    }

    result.outlinePaths.push_back(pathElement);
    ++pathIndex;
  }

  result.ok = true;
  result.outlineGroup = groupElement;
  return result;
}

}  // namespace donner::editor
