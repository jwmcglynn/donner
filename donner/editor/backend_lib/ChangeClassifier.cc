#include "donner/editor/backend_lib/ChangeClassifier.h"

#include <algorithm>
#include <cstddef>

#include "donner/base/xml/XMLNode.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

namespace {

/// Find the byte range that differs between `a` and `b` by scanning from
/// both ends. Returns `{startOffset, endOffsetInOld, endOffsetInNew}`.
/// If the strings are identical, `startOffset == endOffsetInOld == endOffsetInNew`.
struct DiffRange {
  std::size_t changeStart = 0;   ///< First differing byte offset.
  std::size_t changeEndOld = 0;  ///< End of changed region in old string.
  std::size_t changeEndNew = 0;  ///< End of changed region in new string.
};

DiffRange computeDiffRange(std::string_view oldStr, std::string_view newStr) {
  // Scan from the left to find the first differing byte.
  const std::size_t minLen = std::min(oldStr.size(), newStr.size());
  std::size_t left = 0;
  while (left < minLen && oldStr[left] == newStr[left]) {
    ++left;
  }

  if (left == oldStr.size() && left == newStr.size()) {
    // Identical strings.
    return {left, left, left};
  }

  // Scan from the right to find the last differing byte, but don't
  // cross past `left`.
  std::size_t rightOld = oldStr.size();
  std::size_t rightNew = newStr.size();
  while (rightOld > left && rightNew > left && oldStr[rightOld - 1] == newStr[rightNew - 1]) {
    --rightOld;
    --rightNew;
  }

  return {left, rightOld, rightNew};
}

/// Check whether a byte offset falls inside a quoted attribute value in
/// the source text. A simplistic heuristic: walk backward from `offset`
/// looking for an unescaped `"` or `'` that opens a value, then confirm
/// there's a matching close after `offset`.
///
/// Returns the quote positions `[openQuote, closeQuote)` if the offset
/// is inside a quoted value, or `std::nullopt` otherwise.
struct QuotedRange {
  std::size_t openQuote;   ///< Position of the opening quote char.
  std::size_t closeQuote;  ///< Position one past the closing quote char.
};

std::optional<QuotedRange> findEnclosingQuotedValue(std::string_view source, std::size_t offset) {
  if (offset >= source.size()) return std::nullopt;

  // Walk backward to find an opening quote.
  std::size_t pos = offset;
  while (pos > 0) {
    --pos;
    const char c = source[pos];
    if (c == '"' || c == '\'') {
      // Candidate opening quote. Scan forward from pos+1 to find the matching close.
      const char quote = c;
      std::size_t closePos = pos + 1;
      while (closePos < source.size() && source[closePos] != quote) {
        ++closePos;
      }
      if (closePos < source.size() && offset < closePos) {
        // `offset` is between the opening and closing quote.
        return QuotedRange{pos, closePos + 1};
      }
      // The quote we found closes before `offset`, so we're not inside it.
      return std::nullopt;
    }
    // If we hit a tag boundary, stop — we've left the attribute context.
    if (c == '<' || c == '>') return std::nullopt;
  }

  return std::nullopt;
}

/// Find the SVGElement whose source range contains the byte offset `pos`.
/// Walks the SVG element tree (public API) and uses XMLNode::TryCast for
/// source-range lookup. Returns the deepest matching element.
std::optional<svg::SVGElement> findSvgElementAtOffset(const svg::SVGElement& root,
                                                      std::size_t offset) {
  for (auto child = root.firstChild(); child.has_value(); child = child->nextSibling()) {
    auto xmlNode = xml::XMLNode::TryCast(child->entityHandle());
    if (!xmlNode.has_value()) continue;

    auto location = xmlNode->getNodeLocation();
    if (!location.has_value()) continue;
    if (!location->start.offset.has_value() || !location->end.offset.has_value()) continue;

    const std::size_t start = location->start.offset.value();
    const std::size_t end = location->end.offset.value();

    if (offset >= start && offset < end) {
      auto deeper = findSvgElementAtOffset(*child, offset);
      return deeper.has_value() ? deeper : child;
    }
  }
  return std::nullopt;
}

/// Walk backward from `attrValueStart` to find the attribute name. We
/// expect the pattern `name = "value"` with optional whitespace around `=`.
/// Returns the attribute name or empty string_view on failure.
std::string_view extractAttributeName(std::string_view source, std::size_t attrValueStart) {
  // Step back past the opening quote.
  if (attrValueStart == 0) return {};
  std::size_t pos = attrValueStart - 1;

  // Skip whitespace before the opening quote (between = and value).
  while (pos > 0 && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\n' ||
                     source[pos] == '\r')) {
    --pos;
  }

  // Expect '='.
  if (source[pos] != '=') return {};
  if (pos == 0) return {};
  --pos;

  // Skip whitespace before '='.
  while (pos > 0 && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\n' ||
                     source[pos] == '\r')) {
    --pos;
  }

  // Now we should be at the last character of the attribute name.
  // Scan backward to find the start of the name.
  const std::size_t nameEnd = pos + 1;
  auto isNameChar = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == ':' || static_cast<unsigned char>(c) >= 0x80;
  };

  while (pos > 0 && isNameChar(source[pos - 1])) {
    --pos;
  }
  // Check the character at `pos` itself.
  if (!isNameChar(source[pos])) return {};

  return source.substr(pos, nameEnd - pos);
}

}  // namespace

ClassifyResult classifyTextChange(svg::SVGDocument& document, std::string_view oldSource,
                                  std::string_view newSource) {
  // Step 1: find the changed byte range.
  const DiffRange diff = computeDiffRange(oldSource, newSource);
  if (diff.changeStart == diff.changeEndOld && diff.changeStart == diff.changeEndNew) {
    // No change — shouldn't happen, but handle gracefully.
    return {};
  }

  // Step 2: check if the changed range in the OLD source falls entirely
  // inside a quoted attribute value.
  //
  // For pure insertions (changeEndOld == changeStart, nothing deleted),
  // the insert point may be at the closing quote position — the user is
  // appending to the value. The backward scan from `changeStart` would
  // land on the quote itself, which is considered "outside" by the
  // heuristic. In that case, retry from `changeStart - 1`.
  const bool isPureInsert = (diff.changeEndOld == diff.changeStart);
  auto quoted = findEnclosingQuotedValue(oldSource, diff.changeStart);

  if (!quoted.has_value() && isPureInsert && diff.changeStart > 0) {
    quoted = findEnclosingQuotedValue(oldSource, diff.changeStart - 1);
    if (quoted.has_value() && diff.changeStart > quoted->closeQuote - 1) {
      quoted.reset();  // Insert point is past this value's end.
    }
  }

  if (!quoted.has_value()) {
    return {};  // Structural change — outside any attribute value.
  }

  // Verify the entire changed region is inside the same quoted value.
  if (diff.changeEndOld > quoted->closeQuote - 1) {
    return {};  // Change extends past the closing quote.
  }

  // Step 3: find the SVGElement that contains this byte offset.
  auto element = findSvgElementAtOffset(document.svgElement(), diff.changeStart);
  if (!element.has_value()) {
    return {};
  }

  // Step 4: extract the attribute name from the source.
  const std::string_view attrName = extractAttributeName(oldSource, quoted->openQuote);
  if (attrName.empty()) {
    return {};
  }

  // Step 5: extract the new value from the new source. The opening quote
  // is at the same position in the new source (since everything before
  // `changeStart` is identical). The closing quote position shifts by the
  // length delta.
  const std::ptrdiff_t delta =
      static_cast<std::ptrdiff_t>(newSource.size()) - static_cast<std::ptrdiff_t>(oldSource.size());
  const std::size_t newCloseQuote =
      static_cast<std::size_t>(static_cast<std::ptrdiff_t>(quoted->closeQuote - 1) + delta);
  if (newCloseQuote >= newSource.size()) {
    return {};
  }
  if (newSource[newCloseQuote] != oldSource[quoted->closeQuote - 1]) {
    return {};  // The closing quote moved or was deleted — structural.
  }

  // The new value is everything between the opening and closing quotes.
  const std::string_view newValue =
      newSource.substr(quoted->openQuote + 1, newCloseQuote - quoted->openQuote - 1);

  // Step 6: build and return the SetAttribute command.
  return ClassifyResult{
      EditorCommand::SetAttributeCommand(*element, std::string(attrName), std::string(newValue))};
}

}  // namespace donner::editor
