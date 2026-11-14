#pragma once
/// @file

#include <string>
#include <string_view>

namespace donner::css {

/**
 * Serialize @p value as a CSS identifier, per CSSOM "serialize an identifier", so that it reads
 * back as exactly @p value in selector or keyword position. For example `a.b` becomes `a\.b` and
 * `1a` becomes `\31 a`. Bytes 0x80 and above are copied unchanged, so UTF-8 input stays UTF-8.
 *
 * @param value Identifier text, without any `#` or `.` prefix.
 * @return Escaped identifier.
 */
std::string SerializeIdentifier(std::string_view value);

/**
 * Serialize @p value as the contents of a CSS string delimited by @p quote, per CSSOM "serialize
 * a string" but without the surrounding quotes: the quote character, backslash and control
 * characters are escaped.
 *
 * @param value String contents.
 * @param quote Delimiter of the enclosing string, `"` or `'`.
 * @return Escaped contents.
 */
std::string SerializeStringContents(std::string_view value, char quote);

/**
 * Serialize @p value as the value of an unquoted `url(...)` token, escaping every character that
 * would end the token or make it a bad url (whitespace, quotes, parentheses, backslash, control
 * characters).
 *
 * @param value Url text.
 * @return Escaped url token value.
 */
std::string SerializeUrlTokenValue(std::string_view value);

}  // namespace donner::css
