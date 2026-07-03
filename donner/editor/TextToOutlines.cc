#include "donner/editor/TextToOutlines.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/RcString.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGTextElement.h"

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

}  // namespace

ConvertTextToOutlinesResult convertTextToOutlines(svg::SVGDocument& document,
                                                  const svg::SVGElement& textElement) {
  ConvertTextToOutlinesResult result;

  // The conversion only applies to `<text>` elements.
  if (textElement.tagName().name != svg::SVGTextElement::Tag) {
    result.error = "Convert to outlines failed: selection is not a <text> element.";
    return result;
  }

  // Resolve placed glyph outlines via the renderer-facing text geometry. This
  // routes through `TextEngine::computedGlyphPaths()` — the SAME positioned
  // outlines the renderer fills — so the converted paths land exactly where the
  // text rendered. `convertToPath()` is non-const, so operate on a copy of the
  // value-type element handle.
  svg::SVGTextElement text = textElement.cast<svg::SVGTextElement>();
  const std::vector<Path> glyphPaths = text.convertToPath();

  // Empty outlines (missing font, layout failure, empty text) fail the whole
  // conversion without mutating.
  if (glyphPaths.empty()) {
    result.error = "Convert to outlines failed: <text> produced no glyph outlines.";
    return result;
  }
  bool anyNonEmpty = false;
  for (const Path& path : glyphPaths) {
    if (!path.empty()) {
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

  // Build the detached replacement group. Style is preserved at the group
  // level so the outlined geometry inherits the same paint/transform as the
  // original text (fill, fill-rule, opacity, stroke, stroke-width, transform).
  svg::SVGGElement group = svg::SVGGElement::Create(document);
  group.setAttribute("id", groupId);
  group.setAttribute("data-donner-converted-from", "text");
  static constexpr std::array<std::string_view, 6> kPreservedAttributes = {
      "fill", "fill-rule", "opacity", "stroke", "stroke-width", "transform"};
  svg::SVGElement groupElement = group;
  for (const std::string_view name : kPreservedAttributes) {
    copyAttributeIfPresent(textElement, name, groupElement);
  }

  // Build one detached `<path>` per placed glyph, in paint order. Empty glyph
  // paths (e.g. whitespace) are skipped — they contribute no geometry.
  int pathIndex = 0;
  for (const Path& path : glyphPaths) {
    if (path.empty()) {
      continue;
    }
    svg::SVGPathElement pathElement = svg::SVGPathElement::Create(document);
    pathElement.setAttribute("id", groupId + "_" + std::to_string(pathIndex));
    pathElement.setAttribute("d", std::string_view(path.toSVGPathData()));
    result.outlinePaths.push_back(pathElement);
    ++pathIndex;
  }

  result.ok = true;
  result.outlineGroup = groupElement;
  return result;
}

}  // namespace donner::editor
