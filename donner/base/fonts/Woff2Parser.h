#pragma once
/// @file

#include <cstddef>
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
  struct Options {
    size_t maximumInputSize = 16 * 1024 * 1024;
    size_t maximumOutputSize = 64 * 1024 * 1024;
    size_t maximumIntermediateSize = 16 * 1024 * 1024;
    size_t maximumTransformedGlyfSize = 4 * 1024 * 1024;
    size_t maximumTableCount = 4096;
    /// Nonzero for a build-pinned font whose exact reconstructed length is known. Uses one fixed
    /// output vector instead of a growing string followed by a copy; header and result must match.
    size_t expectedOutputSize = 0;
    /// Zero preserves generic decoder allocation behavior. Nonzero bounds all live Brotli
    /// allocations for this decode, including state, buffers and aligned accounting headers.
    size_t maximumBrotliMemory = 0;
    /// Restrict a pinned TrueType-only catalog without weakening generic full-text format support.
    bool requireTrueTypeOutlines = false;
  };

  /**
   * Decompress WOFF2 data into a flat TTF/OTF byte stream.
   *
   * @param woff2Data The WOFF2 file data as a byte span.
   * @return A ParseResult containing the decompressed sfnt bytes on success,
   *         or a ParseDiagnostic on failure.
   */
  static ParseResult<std::vector<uint8_t>> Decompress(std::span<const uint8_t> woff2Data);

  /// Decompress using explicit compressed and expanded byte limits.
  static ParseResult<std::vector<uint8_t>> Decompress(std::span<const uint8_t> woff2Data,
                                                      const Options& options);
};

}  // namespace donner::fonts
