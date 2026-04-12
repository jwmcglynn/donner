#include "donner/base/xml/XMLEscape.h"

#include <cstdint>
#include <string>

namespace donner::xml {

namespace {

/// Return the number of bytes occupied by the UTF-8 codepoint starting at `lead`, or 0
/// for an invalid lead byte.
constexpr int Utf8SequenceLength(std::uint8_t lead) {
  if ((lead & 0x80U) == 0x00U) {
    return 1;
  }
  if ((lead & 0xE0U) == 0xC0U) {
    return 2;
  }
  if ((lead & 0xF0U) == 0xE0U) {
    return 3;
  }
  if ((lead & 0xF8U) == 0xF0U) {
    return 4;
  }
  return 0;  // Continuation byte (10xxxxxx), `0xFE`/`0xFF`, or overlong 5/6-byte form.
}

/// Decode a UTF-8 codepoint of known length starting at `bytes`, validating continuation
/// bytes. Returns the decoded codepoint, or `-1` on any validation failure.
constexpr std::int32_t DecodeUtf8(const std::uint8_t* bytes, int length) {
  // Continuation bytes must all match 10xxxxxx.
  for (int i = 1; i < length; ++i) {
    if ((bytes[i] & 0xC0U) != 0x80U) {
      return -1;
    }
  }

  std::int32_t codepoint = 0;
  switch (length) {
    case 1: codepoint = static_cast<std::int32_t>(bytes[0] & 0x7FU); break;
    case 2:
      codepoint = ((static_cast<std::int32_t>(bytes[0]) & 0x1F) << 6) |
                  (static_cast<std::int32_t>(bytes[1]) & 0x3F);
      break;
    case 3:
      codepoint = ((static_cast<std::int32_t>(bytes[0]) & 0x0F) << 12) |
                  ((static_cast<std::int32_t>(bytes[1]) & 0x3F) << 6) |
                  (static_cast<std::int32_t>(bytes[2]) & 0x3F);
      break;
    case 4:
      codepoint = ((static_cast<std::int32_t>(bytes[0]) & 0x07) << 18) |
                  ((static_cast<std::int32_t>(bytes[1]) & 0x3F) << 12) |
                  ((static_cast<std::int32_t>(bytes[2]) & 0x3F) << 6) |
                  (static_cast<std::int32_t>(bytes[3]) & 0x3F);
      break;
    default: return -1;
  }

  // Reject overlong encodings — the shortest valid form must be used.
  if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
      (length == 4 && codepoint < 0x10000)) {
    return -1;
  }

  // Cap at U+10FFFF.
  if (codepoint > 0x10FFFF) {
    return -1;
  }

  return codepoint;
}

/// Returns true if this codepoint is legal in an XML 1.0 attribute value. The forbidden
/// set is: NUL, most C0 control characters (everything except tab, LF, CR), the UTF-16
/// surrogates, and the two Unicode non-characters U+FFFE and U+FFFF.
constexpr bool IsValidXmlAttributeCodepoint(std::int32_t codepoint) {
  if (codepoint < 0) {
    return false;
  }
  if (codepoint == 0x00) {
    return false;
  }
  if (codepoint < 0x20 && codepoint != 0x09 && codepoint != 0x0A && codepoint != 0x0D) {
    return false;
  }
  if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
    return false;  // UTF-16 surrogate half.
  }
  if (codepoint == 0xFFFE || codepoint == 0xFFFF) {
    return false;
  }
  return true;
}

}  // namespace

std::optional<RcString> EscapeAttributeValue(std::string_view value, char quoteChar) {
  if (quoteChar != '"' && quoteChar != '\'') {
    quoteChar = '"';
  }

  std::string out;
  // Fast path: the escaped output is usually the same length as the input plus a few
  // bytes for the occasional entity. Reserve a little headroom.
  out.reserve(value.size() + 8);

  const auto* const bytes = reinterpret_cast<const std::uint8_t*>(value.data());
  const std::size_t size = value.size();
  std::size_t i = 0;

  while (i < size) {
    const std::uint8_t lead = bytes[i];

    // ASCII fast path: single-byte characters that don't need escaping.
    if (lead < 0x80) {
      const int codepoint = static_cast<int>(lead);
      if (!IsValidXmlAttributeCodepoint(codepoint)) {
        return std::nullopt;
      }
      switch (lead) {
        case '<': out.append("&lt;"); break;
        case '>': out.append("&gt;"); break;
        case '&': out.append("&amp;"); break;
        case '"':
          if (quoteChar == '"') {
            out.append("&quot;");
          } else {
            out.push_back('"');
          }
          break;
        case '\'':
          if (quoteChar == '\'') {
            out.append("&apos;");
          } else {
            out.push_back('\'');
          }
          break;
        // Whitespace that would otherwise be collapsed to a single space by XML
        // attribute-value normalization on the parse side.
        case '\t': out.append("&#9;"); break;
        case '\n': out.append("&#10;"); break;
        case '\r': out.append("&#13;"); break;
        default: out.push_back(static_cast<char>(lead)); break;
      }
      ++i;
      continue;
    }

    // Multi-byte UTF-8: validate and either pass through or reject.
    const int seqLen = Utf8SequenceLength(lead);
    if (seqLen == 0 || i + static_cast<std::size_t>(seqLen) > size) {
      return std::nullopt;
    }
    const std::int32_t codepoint = DecodeUtf8(bytes + i, seqLen);
    if (!IsValidXmlAttributeCodepoint(codepoint)) {
      return std::nullopt;
    }
    out.append(value.substr(i, seqLen));
    i += static_cast<std::size_t>(seqLen);
  }

  return RcString(std::move(out));
}

}  // namespace donner::xml
