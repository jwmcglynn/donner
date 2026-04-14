#pragma once
/// @file

#include <optional>
#include <string_view>

#include "donner/base/RcString.h"

namespace donner::xml {

/**
 * Escape a string for use as an XML attribute value, producing text that round-trips
 * through \ref donner::xml::XMLParser::Parse to recover the original bytes.
 *
 * The output is suitable for splicing between two delimiter characters of the requested
 * \p quoteChar: the returned text does not include the surrounding quote chars, only the
 * escaped value. Caller is responsible for emitting the delimiters.
 *
 * Escape rules:
 * - `<` → `&lt;`, `&` → `&amp;`, `>` → `&gt;`
 * - `"` → `&quot;` when \p quoteChar is `"`, otherwise passthrough
 * - `'` → `&apos;` when \p quoteChar is `'`, otherwise passthrough
 * - `\t`, `\n`, `\r` → numeric character references (`&#9;`, `&#10;`, `&#13;`), so the
 *   parser's attribute-value whitespace normalization does not collapse them into plain
 *   spaces on round-trip.
 * - Valid multi-byte UTF-8 sequences pass through unchanged (we do not percent-encode
 *   non-ASCII bytes, XML attribute values carry UTF-8 natively).
 *
 * Returns `std::nullopt` for input that cannot be represented in a well-formed XML
 * attribute value at all:
 * - The NUL byte (`\0`).
 * - C0 control characters other than `\t`, `\n`, `\r` (i.e. `U+0001`–`U+0008`, `U+000B`,
 *   `U+000C`, `U+000E`–`U+001F`) — these are forbidden in XML 1.0.
 * - Lone surrogates (`U+D800`–`U+DFFF`) encoded in UTF-8.
 * - The non-characters `U+FFFE` and `U+FFFF`.
 * - Overlong UTF-8 sequences or truncated multi-byte starts.
 *
 * This function is total on the *input space it accepts* — any input that makes it
 * through the reject-list above produces a valid escaped string.
 *
 * @param value The raw attribute value bytes.
 * @param quoteChar The quote delimiter the caller will surround the escaped value with.
 *   Must be `'"'` (double quote) or `'\''` (single quote); any other value is treated as
 *   `'"'`.
 * @return The escaped value, or `std::nullopt` if \p value contains characters that
 *   cannot be represented in a well-formed XML attribute value.
 */
std::optional<RcString> EscapeAttributeValue(std::string_view value, char quoteChar = '"');

}  // namespace donner::xml
