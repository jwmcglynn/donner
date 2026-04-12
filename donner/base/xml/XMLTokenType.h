#pragma once
/// @file
///
/// Defines the token types emitted by the XML tokenizer, used for syntax
/// highlighting and source-location-aware editing. See \ref XMLTokenizer.h
/// for the tokenizer API.

#include <cstdint>
#include <ostream>

#include "donner/base/FileOffset.h"

namespace donner::xml {

/**
 * Token types emitted by the XML tokenizer (\ref Tokenize).
 *
 * The token stream is **gap-free**: the concatenation of every token's
 * source range recovers the original input byte-for-byte. No byte is
 * covered by two tokens, and no byte is uncovered (except trailing
 * whitespace after the last element, which is emitted as
 * \ref TextContent).
 */
enum class XMLTokenType : std::uint8_t {
  TagOpen,                ///< `<` (element open) or `</` (closing tag).
  TagName,                ///< Element name, e.g. `rect`, `svg`.
  TagClose,               ///< `>` (end of opening/closing tag).
  TagSelfClose,           ///< `/>` (self-closing element).
  AttributeName,          ///< Attribute name, e.g. `fill`, `xmlns:xlink`.
  AttributeValue,         ///< Quoted attribute value **including** delimiters, e.g. `"red"`.
  Comment,                ///< `<!-- ... -->` (entire comment including delimiters).
  CData,                  ///< `<![CDATA[ ... ]]>` (entire CDATA section).
  TextContent,            ///< Raw text between tags.
  XmlDeclaration,         ///< `<?xml ... ?>` (entire declaration).
  Doctype,                ///< `<!DOCTYPE ...>` (entire doctype).
  EntityRef,              ///< `&amp;`, `&#x20;`, etc. (within text content).
  ProcessingInstruction,  ///< `<?name ...?>` (entire PI).
  Whitespace,             ///< Whitespace inside a tag (between attributes, around `=`).
  ErrorRecovery,          ///< Emitted for regions the tokenizer cannot parse; error
                          ///< recovery skips to the next `<` or `>` and continues.
};

/// Ostream output operator for \ref XMLTokenType.
inline std::ostream& operator<<(std::ostream& os, XMLTokenType type) {
  switch (type) {
    case XMLTokenType::TagOpen: return os << "TagOpen";
    case XMLTokenType::TagName: return os << "TagName";
    case XMLTokenType::TagClose: return os << "TagClose";
    case XMLTokenType::TagSelfClose: return os << "TagSelfClose";
    case XMLTokenType::AttributeName: return os << "AttributeName";
    case XMLTokenType::AttributeValue: return os << "AttributeValue";
    case XMLTokenType::Comment: return os << "Comment";
    case XMLTokenType::CData: return os << "CData";
    case XMLTokenType::TextContent: return os << "TextContent";
    case XMLTokenType::XmlDeclaration: return os << "XmlDeclaration";
    case XMLTokenType::Doctype: return os << "Doctype";
    case XMLTokenType::EntityRef: return os << "EntityRef";
    case XMLTokenType::ProcessingInstruction: return os << "ProcessingInstruction";
    case XMLTokenType::Whitespace: return os << "Whitespace";
    case XMLTokenType::ErrorRecovery: return os << "ErrorRecovery";
  }
  return os << "Unknown";
}

/**
 * A single token emitted by the XML tokenizer.
 */
struct XMLToken {
  XMLTokenType type;   ///< Token type.
  SourceRange range;   ///< Source byte range `[start, end)`.

  /// Convenience: extract the token's text from the original source.
  std::string_view text(std::string_view source) const {
    if (!range.start.offset || !range.end.offset) {
      return {};
    }
    const std::size_t start = range.start.offset.value();
    const std::size_t end = range.end.offset.value();
    if (start >= source.size() || end > source.size() || end < start) {
      return {};
    }
    return source.substr(start, end - start);
  }

  /// Equality operator (for tests).
  bool operator==(const XMLToken& other) const = default;
};

}  // namespace donner::xml
