#include "donner/editor/AttributeWriteback.h"

#include <string>

#include "donner/base/xml/XMLEscape.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"

namespace donner::editor {

std::optional<TextPatch> buildAttributeWriteback(std::string_view source,
                                                  const svg::SVGElement& element,
                                                  std::string_view attrName,
                                                  std::string_view newValue) {
  // Get the element's source start offset so we can locate the attribute.
  const auto xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return std::nullopt;
  }

  const auto nodeLocation = xmlNode->getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value()) {
    return std::nullopt;
  }

  // Escape the value for XML attribute context (double-quoted).
  auto escaped = xml::EscapeAttributeValue(newValue, '"');
  if (!escaped.has_value()) {
    return std::nullopt;
  }

  const xml::XMLQualifiedNameRef qualifiedName(attrName);

  // Try to find the existing attribute in the source text.
  const auto attrRange =
      xml::XMLParser::GetAttributeLocation(source, nodeLocation->start, qualifiedName);

  if (attrRange.has_value()) {
    // Attribute exists — replace its full span (name="value") with the
    // new name="escaped_value".
    if (!attrRange->start.offset.has_value() || !attrRange->end.offset.has_value()) {
      return std::nullopt;
    }

    const std::size_t start = attrRange->start.offset.value();
    const std::size_t end = attrRange->end.offset.value();
    if (start > source.size() || end > source.size() || end < start) {
      return std::nullopt;
    }

    // Build replacement: name="escaped_value"
    std::string replacement;
    replacement.reserve(attrName.size() + 3 + escaped->size());
    replacement.append(attrName);
    replacement.append("=\"");
    replacement.append(std::string_view(*escaped));
    replacement.push_back('"');

    return TextPatch{start, end - start, std::move(replacement)};
  }

  // Attribute doesn't exist — insert ` name="value"` before the element's
  // closing `>` or `/>`. Find the `>` by scanning backward from the
  // element's end offset (or forward from start if end isn't useful).
  //
  // Strategy: scan the source from the element's start offset looking for
  // the first `>` that isn't inside a quoted attribute value. This is the
  // opening tag's close, which is where we insert.
  const std::size_t elementStart = nodeLocation->start.offset.value();

  // Walk from element start looking for the closing `>` of the opening tag.
  // We need to skip over quoted regions.
  std::size_t pos = elementStart;
  if (pos < source.size() && source[pos] == '<') {
    ++pos;  // skip '<'
  }

  // Skip element name.
  while (pos < source.size() && source[pos] != '>' && source[pos] != '/' && source[pos] != ' ' &&
         source[pos] != '\t' && source[pos] != '\n' && source[pos] != '\r') {
    ++pos;
  }

  // Walk through attributes, skipping quoted values.
  while (pos < source.size()) {
    const char c = source[pos];
    if (c == '"' || c == '\'') {
      ++pos;
      while (pos < source.size() && source[pos] != c) {
        ++pos;
      }
      if (pos < source.size()) ++pos;
    } else if (c == '>' || (c == '/' && pos + 1 < source.size() && source[pos + 1] == '>')) {
      // Found the tag close. Insert ` name="value"` right before it.
      std::string insertion;
      insertion.push_back(' ');
      insertion.append(attrName);
      insertion.append("=\"");
      insertion.append(std::string_view(*escaped));
      insertion.push_back('"');

      return TextPatch{pos, 0, std::move(insertion)};
    } else {
      ++pos;
    }
  }

  // Couldn't find the tag close — source is malformed or stale.
  return std::nullopt;
}

}  // namespace donner::editor
