#include "donner/editor/ContentSniffer.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace donner::editor {

namespace {

/// Advance past a UTF-8 BOM and ASCII whitespace, returning the offset
/// of the first "content" byte. Keeps the rest of the heuristic simple
/// — every check can assume `bytes[i]` is the first meaningful char.
std::size_t SkipBomAndWhitespace(std::span<const uint8_t> bytes) {
  std::size_t i = 0;
  if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
    i = 3;
  }
  while (i < bytes.size()) {
    const uint8_t b = bytes[i];
    if (b == ' ' || b == '\t' || b == '\n' || b == '\r') {
      ++i;
      continue;
    }
    break;
  }
  return i;
}

bool StartsWithCaseInsensitive(std::span<const uint8_t> bytes, std::size_t offset,
                               std::string_view needle) {
  if (bytes.size() - offset < needle.size()) {
    return false;
  }
  for (std::size_t k = 0; k < needle.size(); ++k) {
    const char b = static_cast<char>(bytes[offset + k]);
    const char lo = (b >= 'A' && b <= 'Z') ? static_cast<char>(b + ('a' - 'A')) : b;
    if (lo != needle[k]) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::optional<std::string> DescribeNonSvgBytes(std::span<const uint8_t> bytes) {
  if (bytes.empty()) {
    return std::string("Empty response (0 bytes).");
  }

  const std::size_t i = SkipBomAndWhitespace(bytes);
  if (i >= bytes.size()) {
    return std::string("Response was only whitespace.");
  }

  // HTML is the common miss — users paste wiki `/wiki/File:...svg` URLs
  // and get back the file-description page instead of the SVG itself.
  if (StartsWithCaseInsensitive(bytes, i, "<!doctype html") ||
      StartsWithCaseInsensitive(bytes, i, "<html")) {
    return std::string(
        "Got an HTML page, not an SVG. "
        "Check that the URL points directly to a `.svg` file.");
  }

  // We deliberately don't warn on other `<!DOCTYPE ...>` preambles.
  // SVG 1.1 documents commonly carry `<!DOCTYPE svg PUBLIC ...>` and
  // the parser handles them fine; any genuinely non-SVG XML will fall
  // through to the parser's diagnostic, which has better context than
  // a byte-prefix heuristic can offer.

  // JSON / JS arrays.
  if (bytes[i] == '{' || bytes[i] == '[') {
    return std::string("Got JSON, not an SVG.");
  }

  // Empty-looking or plainly non-XML: nothing starts with `<`.
  if (bytes[i] != '<') {
    return std::string(
               "Response doesn't look like XML. "
               "First byte: '") +
           static_cast<char>(std::isprint(bytes[i]) ? bytes[i] : '?') + "'.";
  }

  // `<?xml ...?>` or `<svg ...>` or any other `<...>`-led content:
  // defer to the parser's diagnostic.
  return std::nullopt;
}

}  // namespace donner::editor
