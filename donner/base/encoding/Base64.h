#pragma once
/// @file

#include <vector>

#include "donner/base/ParseResult.h"

namespace donner {

/**
 * Decode a base64-encoded string into a byte array. If the string is not valid base64, an error is
 * returned.
 *
 * @param base64String The base64-encoded string to decode.
 * @return The decoded byte array, or an error if the input is not valid base64.
 */
ParseResult<std::vector<uint8_t>> DecodeBase64Data(std::string_view base64String);

}  // namespace donner
