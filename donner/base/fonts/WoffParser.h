#pragma once
/// @file

#include <cstdint>
#include <span>

#include "donner/base/ParseResult.h"
#include "donner/base/fonts/WoffFont.h"

namespace donner::fonts {

/**
 * Parse a WOFF font file.
 *
 * This parser reads the WOFF format (version 1.0), decompresses the font tables, and returns a
 * WoffFont object containing the parsed data.
 *
 * @see https://www.w3.org/TR/WOFF/ for the WOFF specification.
 *
 * @param bytes The WOFF file data as a byte span.
 * @return A ParseResult containing the parsed WoffFont on success, or a ParseError on failure.
 */
class WoffParser {
public:
  static ParseResult<WoffFont> Parse(std::span<const uint8_t> bytes);
};

}  // namespace donner::fonts
