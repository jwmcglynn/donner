#pragma once
/// @file

#include <cstdint>
#include <string_view>
#include <vector>

#include "donner/base/ParseResult.h"

namespace donner {

/**
 * Decompress gzip-compressed data.
 *
 * @param compressedData Buffer containing gzip-compressed bytes.
 * @return Decompressed data on success, or a ParseError on failure.
 */
ParseResult<std::vector<uint8_t>> DecompressGzip(std::string_view compressedData);

}  // namespace donner
