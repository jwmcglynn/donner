#pragma once
/// @file

#include <cstddef>
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
 * @return A ParseResult containing the parsed WoffFont on success, or a ParseDiagnostic on failure.
 */
class WoffParser {
public:
  struct Options {
    size_t maximumInputSize = 16 * 1024 * 1024;
    size_t maximumTableSize = 30 * 1024 * 1024;
    size_t maximumSfntSize = 64 * 1024 * 1024;
  };

  /**
   * Parse the given WOFF data.
   *
   * @param bytes The WOFF file data as a byte span.
   * @return A ParseResult containing the parsed WoffFont on success, or a ParseDiagnostic on
   * failure.
   */
  static ParseResult<WoffFont> Parse(std::span<const uint8_t> bytes);

  /// Parse using explicit resource limits, primarily for embedding policies and fuzzing.
  static ParseResult<WoffFont> Parse(std::span<const uint8_t> bytes, const Options& options);
};

}  // namespace donner::fonts
