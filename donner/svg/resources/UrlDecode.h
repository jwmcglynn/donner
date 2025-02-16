#pragma once
/// @file

#include <cstddef>
#include <vector>

namespace donner::svg {

/**
 * Decode a URL-encoded string into a byte array, translating `%XX` sequences into the corresponding
 * byte value.
 *
 * @see https://url.spec.whatwg.org/#percent-encoded-bytes
 *
 * @param urlEncodedString The URL-encoded string to decode.
 * @return A vector of decoded byte values.
 */
std::vector<uint8_t> UrlDecode(std::string_view urlEncodedString);

}  // namespace donner::svg
