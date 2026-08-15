#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "donner/base/ParseResult.h"

namespace donner {

/**
 * A utility class for decompressing data.
 */
class Decompress {
public:
  /// Default maximum expanded size for gzip streams accepted from untrusted inputs.
  static constexpr size_t kDefaultMaximumOutputSize = 16 * 1024 * 1024;

  /**
   * Decompress gzip-compressed data.
   *
   * @param compressedData Buffer containing gzip-compressed bytes.
   * @param maximumOutputSize Maximum number of decompressed bytes to produce.
   * @return Decompressed data on success, or a ParseDiagnostic on failure.
   */
  static ParseResult<std::vector<uint8_t>> Gzip(
      std::string_view compressedData, size_t maximumOutputSize = kDefaultMaximumOutputSize);

  /**
   * Decompress zlib-compressed data.
   *
   * @param compressedData Buffer containing zlib-compressed bytes.
   * @param decompressedSize The expected size of the decompressed data.
   * @return Decompressed data on success, or a ParseDiagnostic on failure.
   */
  static ParseResult<std::vector<uint8_t>> Zlib(std::string_view compressedData,
                                                size_t decompressedSize);
};

}  // namespace donner
