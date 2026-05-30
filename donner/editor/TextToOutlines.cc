#include "donner/editor/TextToOutlines.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/Path.h"
#include "donner/base/RcString.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/SVGTextElement.h"

namespace donner::editor {

namespace {

/// Resolve a FileOffset against \p source to an absolute byte offset.
std::optional<std::size_t> ResolveFileOffset(std::string_view source, const FileOffset& offset) {
  const FileOffset resolved = offset.resolveOffset(source);
  if (!resolved.offset.has_value()) {
    return std::nullopt;
  }
  return std::min(*resolved.offset, source.size());
}

/// Half-open `[start, end)` byte range of \p element's serialized node in
/// \p source, or std::nullopt if the element has no source mapping. Mirrors the
/// helper in `ShapeClipboardCommands.cc`.
std::optional<std::pair<std::size_t, std::size_t>> elementSourceRange(
    const svg::SVGElement& element, std::string_view source) {
  return element.withReadAccess(
      [source](svg::DocumentReadAccess&,
               EntityHandle handle) -> std::optional<std::pair<std::size_t, std::size_t>> {
        auto xmlNode = xml::XMLNode::TryCast(handle);
        if (!xmlNode.has_value()) {
          return std::nullopt;
        }
        auto range = xmlNode->getNodeLocation();
        if (!range.has_value()) {
          return std::nullopt;
        }
        const std::optional<std::size_t> start = ResolveFileOffset(source, range->start);
        const std::optional<std::size_t> end = ResolveFileOffset(source, range->end);
        if (!start.has_value() || !end.has_value() || *end <= *start) {
          return std::nullopt;
        }
        return std::make_pair(*start, *end);
      });
}

/// Escape \p value for inclusion in a double-quoted XML attribute. `d=` glyph
/// path data and the style values we copy only ever contain ASCII path/style
/// syntax, but we escape defensively so an authored `transform` carrying `&`,
/// `<`, `>`, or `"` round-trips through the reparse.
std::string escapeAttributeValue(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '&': out.append("&amp;"); break;
      case '<': out.append("&lt;"); break;
      case '>': out.append("&gt;"); break;
      case '"': out.append("&quot;"); break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

/// Append `name="value"` to \p out (prefixed by a space) when \p textElement
/// carries the authored presentation attribute \p name. Returns true if the
/// attribute was present and copied.
bool copyAttributeIfPresent(const svg::SVGElement& textElement, std::string_view name,
                            std::string& out) {
  const std::optional<RcString> value = textElement.getAttribute(xml::XMLQualifiedNameRef(name));
  if (!value.has_value()) {
    return false;
  }
  out.push_back(' ');
  out.append(name);
  out.append("=\"");
  out.append(escapeAttributeValue(value->str()));
  out.push_back('"');
  return true;
}

}  // namespace

ConvertTextToOutlinesResult convertTextToOutlines(const svg::SVGDocument& document,
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

  // Per design doc § "Error Handling": empty outlines (missing font, layout
  // failure, empty text) fail the whole conversion without mutating.
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

  // Locate the `<text>` node's source range so we can splice the replacement
  // group in situ (preserving paint order). A source-less element cannot be
  // converted through the reparse path.
  const std::string_view source = document.source();
  if (source.empty()) {
    result.error = "Convert to outlines failed: document has no source text.";
    return result;
  }
  const std::optional<std::pair<std::size_t, std::size_t>> range =
      elementSourceRange(textElement, source);
  if (!range.has_value()) {
    result.error = "Convert to outlines failed: <text> element has no source mapping.";
    return result;
  }

  // Group id: base it on the text element's id when present, else a stable name.
  const RcString textId = textElement.id();
  const std::string groupId =
      textId.empty() ? std::string("text_outlines") : (textId.str() + "_outlines");
  result.outlineGroupId = groupId;

  // Build the replacement group. Style is preserved at the group level so the
  // outlined geometry inherits the same paint/transform as the original text
  // (per design doc § "Convert Text to Outlines" step 5 and § "Requirements":
  // fill, fill-rule, opacity, stroke, stroke-width, transform).
  std::string group = "<g id=\"";
  group.append(escapeAttributeValue(groupId));
  group.push_back('"');
  group.append(" data-donner-converted-from=\"text\"");
  static constexpr std::array<std::string_view, 6> kPreservedAttributes = {
      "fill", "fill-rule", "opacity", "stroke", "stroke-width", "transform"};
  for (const std::string_view name : kPreservedAttributes) {
    copyAttributeIfPresent(textElement, name, group);
  }
  group.push_back('>');
  group.push_back('\n');

  // Emit one `<path>` per placed glyph, in paint order. Empty glyph paths
  // (e.g. whitespace) are skipped — they contribute no geometry.
  int pathIndex = 0;
  for (const Path& path : glyphPaths) {
    if (path.empty()) {
      continue;
    }
    const RcString d = path.toSVGPathData();
    group.append("  <path id=\"");
    group.append(escapeAttributeValue(groupId));
    group.push_back('_');
    group.append(std::to_string(pathIndex));
    group.append("\" d=\"");
    group.append(escapeAttributeValue(d.str()));
    group.append("\"/>\n");
    ++pathIndex;
  }
  group.append("</g>");

  // Splice: replace the `<text>` node's source range with the outline group.
  // Removing the original `<text>` happens only here, after outline generation
  // succeeded (design doc step 6 / § "Error Handling": no partial mutation).
  std::string merged(source);
  merged.replace(range->first, range->second - range->first, group);

  result.ok = true;
  result.mergedSource = std::move(merged);
  return result;
}

}  // namespace donner::editor
