#pragma once
/// @file

#include <cstdint>
#include <span>
#include <vector>

#include "donner/base/ParseResult.h"

namespace donner::fonts {

/**
 * Decompress a WOFF2 font file into a raw TTF/OTF byte stream.
 *
 * WOFF2 fonts use Brotli compression and a custom table transform to achieve higher
 * compression ratios than WOFF 1.0. This parser delegates to the Google woff2 library
 * (decode-only) to reconstruct the sfnt byte stream.
 *
 * @see https://www.w3.org/TR/WOFF2/ for the WOFF2 specification.
 */
class Woff2Parser {
public:
  /**
   * Decompress WOFF2 data into a flat TTF/OTF byte stream.
   *
   * @param woff2Data The WOFF2 file data as a byte span.
   * @return A ParseResult containing the decompressed sfnt bytes on success,
   *         or a ParseError on failure.
   */
  static ParseResult<std::vector<uint8_t>> Decompress(std::span<const uint8_t> woff2Data);
};

}  // namespace donner::fonts
