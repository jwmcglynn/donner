#pragma once
/// @file

#include <span>

#include "src/base/parser/parse_result.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

/**
 * Parse an SVG XML document.
 */
class XMLParser {
public:
  /**
   * Parses an SVG XML document (typically the contents of a .svg file).
   *
   * To reduce copying, the input buffer is modified to produce substrings, so it must be mutable
   * and end with a '\0'.
   *
   * @param str Mutable input data, which must be mutable and null-terminated.
   * @param[out] outWarnings If non-null, append warnings encountered to this vector.
   * @return Parsed SVGDocument, or an error if a fatal error is encountered.
   */
  static ParseResult<SVGDocument> ParseSVG(std::span<char> str,
                                           std::vector<ParseError>* outWarnings = nullptr) noexcept;
};

}  // namespace donner::svg
