#pragma once
/// @file

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
  /**
   * Decompress gzip-compressed data.
   *
   * @param compressedData Buffer containing gzip-compressed bytes.
   * @return Decompressed data on success, or a ParseError on failure.
   */
  static ParseResult<std::vector<uint8_t>> Gzip(std::string_view compressedData);

  /**
   * Decompress zlib-compressed data.
   *
   * @param compressedData Buffer containing zlib-compressed bytes.
   * @param decompressedSize The expected size of the decompressed data.
   * @return Decompressed data on success, or a ParseError on failure.
   */
  static ParseResult<std::vector<uint8_t>> Zlib(std::string_view compressedData,
                                                size_t decompressedSize);
};

}  // namespace donner
