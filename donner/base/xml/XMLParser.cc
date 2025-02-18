#include "donner/base/xml/XMLParser.h"

#include <cassert>  // For assert
#include <cstddef>
#include <cstdlib>  // For std::size_t
#include <string_view>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseError.h"
#include "donner/base/Utf8.h"
#include "donner/base/Utils.h"
#include "donner/base/parser/IntegerParser.h"
#include "donner/base/parser/LineOffsets.h"
#include "donner/base/parser/details/ParserBase.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLQualifiedName.h"

namespace donner::xml {

using donner::ParseError;
using donner::ParseResult;

namespace {

// LCOV_EXCL_START: Compile-time only
template <typename Lambda>
constexpr std::array<unsigned char, 256> BuildLookupTable(Lambda lambda) {
  std::array<unsigned char, 256> table = {};
  for (int i = 0; i < 256; ++i) {
    if (lambda(static_cast<char>(i))) {
      table[i] = 1;
    } else {
      table[i] = 0;
    }
  }

  return table;
}
// LCOV_EXCL_STOP

class XMLParserImpl : public parser::ParserBase {
private:
  XMLDocument document_;
  XMLParser::Options options_;
  std::optional<parser::LineOffsets> lineOffsets_;

public:
  explicit XMLParserImpl(std::string_view text, const XMLParser::Options& options)
      : parser::ParserBase(text), options_(options) {}

  ParseResult<XMLDocument> parse() {
    if (str_.empty()) {
      return document_;
    }

    assert(!remaining_.empty() && "parse() already called");

    // Detect and skip the BOM, if it exists
    parseBOM();

    // Parse children
    while (true) {
      // Skip whitespace before node
      skipWhitespace();
      if (remaining_.empty() || peek() == '\0') {
        break;
      }

      const FileOffset startOffset = currentOffsetWithLineNumber();

      // Parse and append new child
      if (tryConsume("<")) {
        auto maybeNode = parseNode(startOffset);

        if (maybeNode.hasError()) {
          return std::move(maybeNode.error());
        }

        if (maybeNode.result().has_value()) {
          document_.root().appendChild(maybeNode.result().value());
        }
      } else {
        return createParseError("Expected '<' to start a node");
      }
    }

    return document_;
  }

  std::optional<FileOffsetRange> getElementAttributeLocation(const XMLQualifiedNameRef& name) {
    UTILS_RELEASE_ASSERT_MSG(tryConsume("<"), "Expected element to start with '<'");

    // Extract element name
    auto maybeName = consumeQualifiedName();
    UTILS_RELEASE_ASSERT_MSG(!maybeName.hasError(),
                             "Expected element to have previously parsed correctly");

    // Skip whitespace between element name and attributes
    skipWhitespace();

    while (true) {
      const FileOffset attributeStartOffset = currentOffsetWithLineNumber();

      ParseResult<std::optional<ParsedAttribute>> maybeAttribute = parseNextAttribute();
      UTILS_RELEASE_ASSERT_MSG(!maybeAttribute.hasError(),
                               "Expected element to have previously parsed correctly");

      const FileOffset attributeEndOffset = currentOffsetWithLineNumber();
      skipWhitespace();

      if (!maybeAttribute.result().has_value()) {
        break;
      }

      const ParsedAttribute& attribute = maybeAttribute.result().value();
      if (attribute.name == name) {
        return FileOffsetRange{attributeStartOffset, attributeEndOffset};
      }
    }

    return std::nullopt;
  }

private:
  parser::LineOffsets lineOffsets() {
    if (!lineOffsets_) {
      lineOffsets_ = parser::LineOffsets(str_);
    }

    return lineOffsets_.value();
  }

  FileOffset currentOffsetWithLineNumber(int relativeOffset = 0) {
    const size_t offset = currentOffset(relativeOffset).offset.value();
    return lineOffsets().fileOffset(offset);
  }

  ParseError createParseError(std::string_view reason) {
    ParseError result;
    result.reason = reason;
    result.location = currentOffsetWithLineNumber();
    return result;
  }

  ParseError mapParseError(ParseError&& error, int relativeOffset = 0) {
    ParseError result = std::move(error);
    result.location = result.location.addParentOffset(currentOffsetWithLineNumber(relativeOffset));
    return result;
  }

  // Detects qualified name characters, e.g. element or attribute names, which may contain a colon
  // if they have a namespace prefix
  struct NamePredicate {
    /// Valid names (anything but space `\n` `\r` `\t` `/` `<` `>` `=` `?` `!` `\0`)
    static constexpr std::array<unsigned char, 256> kLookupName = BuildLookupTable([](char ch) {
      return !(ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '/' || ch == '<' ||
               ch == '>' || ch == '=' || ch == '?' || ch == '!' || ch == '\0');
    });

    static unsigned char test(char ch) { return kLookupName[static_cast<unsigned char>(ch)]; }
  };

  // Detects attribute name characters without ':', which may be a namespace prefix or local name
  struct NameNoColonPredicate {
    /// Name without colon (anything but space `\n` `\r` `\t` `/` `<` `>` `=` `?` `!` `\0`
    /// `:`)
    static constexpr std::array<unsigned char, 256> kLookupNameNoColon =
        BuildLookupTable([](char ch) {
          return !(ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '/' || ch == '<' ||
                   ch == '>' || ch == '=' || ch == '?' || ch == '!' || ch == '\0' || ch == ':');
        });

    static unsigned char test(char ch) {
      return kLookupNameNoColon[static_cast<unsigned char>(ch)];
    }
  };

  /// Detects text data between nodes, e.g. between <tag> and </tag>, including entities (anything
  /// but `<` `\0`)
  struct TextPredicate {
    /// Text (i.e. PCDATA) (anything but `<` `\0`)
    static constexpr std::array<unsigned char, 256> kLookupText =
        BuildLookupTable([](char ch) { return ch != '<' && ch != '\0'; });

    static bool test(char ch) { return kLookupText[static_cast<unsigned char>(ch)] != 0; }
  };

  /// Detects text data within nodes, e.g. between <tag> and </tag> which does not require
  /// reprocessing (anything but `<` `\0` `&`)
  struct TextNoEntityPredicate {
    /// Text (i.e. PCDATA) that does not require reprocessing (anything but `<` `\0` `&`)
    static constexpr std::array<unsigned char, 256> kLookupTextNoEntity =
        BuildLookupTable([](char ch) { return ch != '<' && ch != '\0' && ch != '&'; });

    static bool test(char ch) { return kLookupTextNoEntity[static_cast<unsigned char>(ch)] != 0; }
  };

  /// Matches quoted attribute value characters (any character except `\0` or the closing quote)
  template <char Quote>
  struct QuotedStringPredicate {
    /// Quoted string contents (anything but the quote and `\0`)
    static constexpr std::array<unsigned char, 256> kLookupStringData =
        BuildLookupTable([](char ch) { return ch != Quote && ch != '\0'; });

    static bool test(char ch) { return kLookupStringData[static_cast<unsigned char>(ch)] != 0; }
  };

  /// Matches quoted attribute value characters except entity references (e.g. `&amp;`), any
  /// character except `&`, `\0`, or the closing quote.
  template <char Quote>
  struct QuotedStringNoEntityPredicate {
    /// Quoted string contents which does not require processing (anything but the quote and `\0`
    /// `&`)
    static constexpr std::array<unsigned char, 256> kLookupStringDataNoEntity =
        BuildLookupTable([](char ch) { return ch != Quote && ch != '\0' && ch != '&'; });

    static bool test(char ch) {
      return kLookupStringDataNoEntity[static_cast<unsigned char>(ch)] != 0;
    }
  };

  /**
   * Insert a decoded unicode character, an integer in the range [0, 0x10FFFF], and inserts Utf8
   * codepoints into the \c it output iterator.
   *
   * @param ch Unicode character to insert, in the range [0, 0x10FFFF].
   * @param it Output iterator to insert UTF-8 codepoints.
   * @return An error if the character is invalid, or \c std::nullopt if the character was inserted
   */
  template <std::output_iterator<char> OutputIterator>
  [[nodiscard]] std::optional<ParseError> insertUtf8(char32_t ch, OutputIterator it) {
    // Reject bad codepoints as defined by https://www.w3.org/TR/xml/#NT-Char
    if (UTILS_PREDICT_FALSE(!Utf8::IsValidCodepoint(ch) || ch == 0xFFFE || ch == 0xFFFF)) {
      return createParseError("Invalid numeric character entity");
    }

    Utf8::Append(ch, it);
    return std::nullopt;
  }

  // Skip whitespace characters
  void skipWhitespace() {
    size_t skipCount = 0;
    const size_t len = remaining_.size();

    while (skipCount < len && isWhitespace(remaining_[skipCount])) {
      ++skipCount;
    }

    remaining_.remove_prefix(skipCount);
  }

  // Consume and return a substring while a predicate evaluates to true
  template <class MatchPredicate>
  std::string_view consumeMatching() {
    size_t i = 0;
    const size_t len = remaining_.size();

    while (i < len && MatchPredicate::test(remaining_[i])) {
      ++i;
    }

    const std::string_view result = remaining_.substr(0, i);
    remaining_.remove_prefix(i);
    return result;
  }

  // Skip characters until predicate evaluates to true while doing the following:
  // - replacing XML character entity references with proper characters (&apos; &amp; &quot; &lt;
  // &gt; &#...;)
  template <class MatchPredicate, class MatchPredicateNoEntity>
  ParseResult<RcStringOrRef> consumeAndExpandEntities() {
    using donner::parser::IntegerParser;

    if (UTILS_PREDICT_FALSE(options_.disableEntityTranslation)) {
      return ParseResult<RcStringOrRef>(consumeMatching<MatchPredicate>());
    }

    // Consumes until the first modification is detected.
    const std::string_view unmodifiedContents = consumeMatching<MatchPredicateNoEntity>();

    // If the entire string was unmodified, return it. Otherwise we need to do entity conversion.
    if (!MatchPredicate::test(peek().value_or('\0'))) {
      return RcStringOrRef(unmodifiedContents);
    }

    std::vector<char> dest(unmodifiedContents.begin(), unmodifiedContents.end());

    // Use translation skip
    while (MatchPredicate::test(peek().value_or('\0'))) {
      // TODO: Switch to compile-time trie
      if (peek() == '&') {
        if (tryConsume("&amp;")) {
          dest.push_back('&');
        } else if (tryConsume("&apos;")) {
          dest.push_back('\'');
        } else if (tryConsume("&quot;")) {
          dest.push_back('\"');
        } else if (tryConsume("&lt;")) {
          dest.push_back('<');
        } else if (tryConsume("&gt;")) {
          dest.push_back('>');
        } else if (tryConsume("&#")) {
          std::vector<char> result;

          if (peek() == 'x') {
            auto maybeResult = IntegerParser::ParseHex(remaining_.substr(1));
            if (maybeResult.hasError()) {
              return mapParseError(std::move(maybeResult.error()), /*relativeOffset=*/1);
            }

            const IntegerParser::Result integerResult = maybeResult.result();
            // Decodes and adds to output text.
            if (auto maybeError = insertUtf8(integerResult.number, std::back_inserter(dest))) {
              return std::move(maybeError.value());
            }

            remaining_.remove_prefix(integerResult.consumedChars + 1);
          } else {
            auto maybeResult = IntegerParser::Parse(remaining_);
            if (maybeResult.hasError()) {
              return mapParseError(std::move(maybeResult.error()));
            }

            const IntegerParser::Result integerResult = maybeResult.result();

            // Decodes and adds to output text.
            if (auto maybeError = insertUtf8(integerResult.number, std::back_inserter(dest))) {
              return std::move(maybeError.value());
            }

            remaining_.remove_prefix(integerResult.consumedChars);
          }

          if (!tryConsume(";")) {
            return createParseError("Numeric character entity missing closing ';'");
          }
        } else {
          // No matches, copy '&' directly
          dest.push_back('&');
          remaining_.remove_prefix(1);
        }
      } else {
        // Regular character, copy
        dest.push_back(remaining_[0]);
        remaining_.remove_prefix(1);
      }
    }

    return RcStringOrRef(RcString::fromVector(std::move(dest)));
  }

  bool tryConsume(std::string_view token) {
    if (remaining_.starts_with(token)) {
      remaining_.remove_prefix(token.size());
      return true;
    }

    return false;
  }

  [[nodiscard]] std::optional<char> peek() {
    if (remaining_.empty()) {
      return std::nullopt;
    }

    return remaining_[0];
  };

  // Parse BOM, if any
  void parseBOM() {
    // UTF-8?
    (void)tryConsume("\xEF\xBB\xBF");  // Skip utf-8 bom
  }

  /**
   * Consume text until given substring is found and removed.
   *
   * For example:
   *   "abc-->123".consumeAndConsume("-->") -> "abc", where remaining_ = "123"
   *
   * If the substring is not found, returns \c std::nullopt and \ref remaining_ is left unchanged.
   *
   * @param substr Substring to search for.
   * @return The text before the substring, or \c std::nullopt if the substring was not found.
   */
  std::optional<std::string_view> consumeContentsUntilEndString(std::string_view endString) {
    auto it = remaining_.find(endString);
    if (it == std::string_view::npos) {
      return std::nullopt;
    }

    std::string_view result = remaining_.substr(0, it);
    remaining_.remove_prefix(it + endString.size());
    return result;
  }

  // Parse XML declaration (<?xml...)
  ParseResult<XMLNode> parseXMLDeclaration(FileOffset startOffset) {
    // Create declaration
    XMLNode declaration = XMLNode::CreateXMLDeclarationNode(document_);
    declaration.setSourceStartOffset(startOffset);

    // Skip whitespace before attributes or ?>
    skipWhitespace();

    // Parse declaration attributes
    if (auto maybeError = parseNodeAttributes(declaration)) {
      return std::move(maybeError.value());
    }

    // Skip ?>
    if (!tryConsume("?>")) {
      return createParseError("XML declaration missing closing '?>'");
    }

    declaration.setSourceEndOffset(currentOffsetWithLineNumber());
    return declaration;
  }

  // Parse XML comment (<!--...)
  ParseResult<std::optional<XMLNode>> parseComment(FileOffset startOffset) {
    const auto maybeComment = consumeContentsUntilEndString("-->");
    if (!maybeComment) {
      return createParseError("Comment node does not end with '-->'");
    }

    const std::string_view commentStr = maybeComment.value();

    // If Comment nodes are enabled
    if (options_.parseComments) {
      XMLNode commentNode = XMLNode::CreateCommentNode(document_, commentStr);
      commentNode.setSourceStartOffset(startOffset);
      commentNode.setSourceEndOffset(currentOffsetWithLineNumber());
      return std::make_optional(commentNode);
    } else {
      return std::optional<XMLNode>(std::nullopt);
    }
  }

  // Parse DOCTYPE
  ParseResult<std::optional<XMLNode>> parseDoctype(FileOffset startOffset) {
    // Process until '>'
    size_t i = 0;
    size_t len = remaining_.size();
    bool stop = false;

    while (i < len && !stop) {
      switch (remaining_[i]) {
        case '>': {
          // End of doctype node.
          stop = true;
          break;
        }
        case '[': {
          ++i;  // Skip '['

          int depth = 1;
          while (depth > 0) {
            if (i >= len) {
              return createParseError("Doctype node missing closing ']'");
            }

            switch (remaining_[i]) {
              case '[': ++depth; break;
              case ']': --depth; break;
              default: break;
            }

            ++i;
          }
          break;
        }
        case '\0': {
          return createParseError("Unexpected end of data, found embedded null character");
        }
        default: {
          ++i;
          break;
        }
      }
    }

    if (!stop) {
      return createParseError("Doctype node missing closing '>'");
    }

    const std::string_view doctypeStr = remaining_.substr(0, i);
    remaining_.remove_prefix(i + 1);  // Remove contents plus '>'

    if (options_.parseDoctype) {
      // Create a new doctype node
      XMLNode doctype = XMLNode::CreateDocTypeNode(document_, doctypeStr);
      doctype.setSourceStartOffset(startOffset);
      doctype.setSourceEndOffset(currentOffsetWithLineNumber());
      return std::make_optional(doctype);
    } else {
      return std::optional<XMLNode>(std::nullopt);
    }
  }

  // Parse PI nodes, e.g. `<?php ... ?>`
  ParseResult<std::optional<XMLNode>> parseProcessingInstructions(FileOffset startOffset) {
    // Extract PI target name
    const std::string_view piName = consumeMatching<NamePredicate>();
    if (piName.empty()) {
      return createParseError("PI target does not begin with a name, e.g. '<?tag'");
    }

    // Skip whitespace after the PI name.
    skipWhitespace();

    // Consume contents until finding a '?>'
    const auto maybePiValue = consumeContentsUntilEndString("?>");
    if (!maybePiValue) {
      return createParseError("PI node does not end with '?>'");
    }

    const std::string_view piValue = maybePiValue.value();

    if (options_.parseProcessingInstructions) {
      XMLNode pi = XMLNode::CreateProcessingInstructionNode(document_, piName, piValue);
      pi.setSourceStartOffset(startOffset);
      pi.setSourceEndOffset(currentOffsetWithLineNumber());
      return std::make_optional(pi);
    } else {
      return std::optional<XMLNode>(std::nullopt);
    }
  }

  // Parse and append data
  std::optional<ParseError> parseAndAppendData(XMLNode& node) {
    // Skip until end of data
    auto maybeData = consumeAndExpandEntities<TextPredicate, TextNoEntityPredicate>();
    if (maybeData.hasError()) {
      return maybeData.error();
    }

    const RcStringOrRef& dataStr = maybeData.result();

    // Create new data node
    if (!dataStr.empty()) {
      XMLNode data = XMLNode::CreateDataNode(document_, dataStr);
      node.appendChild(data);
    }

    // Add data to parent node if no data exists yet
    if (!node.value().has_value()) {
      node.setValue(dataStr);
    }

    // Return character that ends data
    return std::nullopt;
  }

  // Parse CDATA
  ParseResult<XMLNode> parseCData(FileOffset startOffset) {
    auto maybeCData = consumeContentsUntilEndString("]]>");
    if (!maybeCData) {
      return createParseError("CDATA node does not end with ']]>'");
    }

    const std::string_view cdataStr = maybeCData.value();

    XMLNode cdata = XMLNode::CreateCDataNode(document_, cdataStr);
    cdata.setSourceStartOffset(startOffset);
    cdata.setSourceEndOffset(currentOffsetWithLineNumber());
    return cdata;
  }

  // Parse element node
  ParseResult<XMLNode> parseElement(FileOffset startOffset) {
    // Extract element name
    auto maybeName = consumeQualifiedName();
    if (maybeName.hasError()) {
      ParseError err;
      err.reason = "Invalid element name: " + maybeName.error().reason;
      err.location = maybeName.error().location;
      return err;
    }

    // Create element node
    XMLNode element = XMLNode::CreateElementNode(document_, maybeName.result());
    element.setSourceStartOffset(startOffset);

    // Skip whitespace between element name and attributes or >
    skipWhitespace();

    // Parse attributes, if any
    if (auto maybeError = parseNodeAttributes(element)) {
      return std::move(maybeError.value());
    }

    // Determine ending type
    if (tryConsume(">")) {
      if (auto maybeError = parseNodeContents(element)) {
        return std::move(maybeError.value());
      }

    } else if (tryConsume("/>")) {
      // Self-closing tag
    } else {
      return createParseError("Node not closed with '>' or '/>'");
    }

    element.setSourceEndOffset(currentOffsetWithLineNumber());
    return element;
  }

  // Parse node
  ParseResult<std::optional<XMLNode>> parseNode(FileOffset startOffset) {
    // Parse proper node type
    switch (peek().value_or('\0')) {
      default:
        // Parse and append element node
        return parseElement(startOffset).template map<std::optional<XMLNode>>([](auto result) {
          return std::make_optional(result);
        });

      case '?':
        remaining_.remove_prefix(1);  // Skip '?'
        if (tryConsume("xml")) {
          // '<?xml ' - XML declaration
          return parseXMLDeclaration(startOffset)
              .template map<std::optional<XMLNode>>(
                  [](auto result) { return std::make_optional(result); });
        } else {
          // Parse PI
          return parseProcessingInstructions(startOffset);
        }

        UTILS_UNREACHABLE();  // All cases above should return.

      case '!':
        if (tryConsume("!--")) {
          // '<!--' - XML comment
          return parseComment(startOffset);
        } else if (tryConsume("![CDATA[")) {
          // '<![CDATA[' - CDATA
          return parseCData(startOffset).template map<std::optional<XMLNode>>([](auto result) {
            return std::make_optional(result);
          });
        } else if (tryConsume("!DOCTYPE")) {
          // '<!DOCTYPE' - DOCTYPE

          if (!isWhitespace(peek().value_or('\0'))) {
            return createParseError("Expected whitespace after '<!DOCTYPE'");
          }

          skipWhitespace();
          return parseDoctype(startOffset);
        } else {
          return createParseError("Unrecognized node starting with '<!'");
        }

        UTILS_UNREACHABLE();  // All cases above should return.
    }
  }

  ParseResult<XMLQualifiedNameRef> consumeQualifiedName() {
    const std::string_view name = consumeMatching<NameNoColonPredicate>();
    if (name.empty()) {
      return createParseError("Expected qualified name, found invalid character");
    }

    if (tryConsume(":")) {
      // Namespace prefix found
      const std::string_view localName = consumeMatching<NameNoColonPredicate>();
      if (localName.empty()) {
        return createParseError("Expected local part of name after ':', found invalid character");
      }

      return XMLQualifiedNameRef(name, localName);
    } else {
      return XMLQualifiedNameRef(name);
    }
  }

  // Parse contents of the node
  [[nodiscard]] std::optional<ParseError> parseNodeContents(XMLNode& node) {
    // For all children and text
    while (true) {
      // Skip whitespace between > and node contents
      std::string_view contentsStart = remaining_;
      skipWhitespace();
      std::optional<char> nextChar = peek();

      if (!nextChar || nextChar == '\0') {
        return createParseError("Unexpected end of data parsing node contents");
      }

      if (nextChar == '<') {
        if (tryConsume("</")) {  // Node closing
          const std::string_view closingTagStart = remaining_;

          auto maybeClosingName = consumeQualifiedName();
          if (maybeClosingName.hasError()) {
            ParseError err;
            err.reason = "Invalid closing tag name: " + maybeClosingName.error().reason;
            err.location = maybeClosingName.error().location;
            return err;
          }

          if (node.tagName() != maybeClosingName.result()) {
            remaining_ = closingTagStart;
            return createParseError("Mismatched closing tag");
          }

          skipWhitespace();

          if (!tryConsume(">")) {
            return createParseError("Expected '>' for closing tag");
          }

          return std::nullopt;  // Node closed, finished parsing contents
        } else {
          FileOffset startOffset = currentOffsetWithLineNumber();

          // Child node
          remaining_.remove_prefix(1);  // Skip '<'

          auto maybeNode = parseNode(startOffset);
          if (maybeNode.hasError()) {
            return maybeNode.error();  // Propagate error
          }

          if (maybeNode.result().has_value()) {
            node.appendChild(maybeNode.result().value());
          }
        }
      } else {
        // Data node
        remaining_ = contentsStart;

        if (auto maybeError = parseAndAppendData(node)) {
          return maybeError;  // Propagate error
        }
      }
    }
  }

  struct ParsedAttribute {
    XMLQualifiedNameRef name;
    RcStringOrRef value;
  };

  ParseResult<std::optional<ParsedAttribute>> parseNextAttribute() {
    if (!NameNoColonPredicate::test(peek().value_or('\0'))) {
      // No more attributes to parse.
      return std::optional<ParsedAttribute>(std::nullopt);
    }

    auto maybeName = consumeQualifiedName();
    if (maybeName.hasError()) {
      ParseError err;
      err.reason = "Invalid attribute name: " + maybeName.error().reason;
      err.location = maybeName.error().location;
      return err;
    }

    const XMLQualifiedNameRef& name = maybeName.result();

    // Skip whitespace after attribute name
    skipWhitespace();

    // Skip =
    if (!tryConsume("=")) {
      return createParseError("Attribute name without value, expected '=' followed by a string");
    }

    // Skip whitespace after =
    skipWhitespace();

    // Skip quote and remember if it was ' or "
    auto maybeQuote = peek();
    if (!maybeQuote || (*maybeQuote != '\'' && *maybeQuote != '"')) {
      return createParseError("Attribute value not enclosed in quotes, expected \" or '");
    }

    const char quote = maybeQuote.value();
    remaining_.remove_prefix(1);

    // Extract attribute value and expand char refs in it
    ParseResult<RcStringOrRef> maybeValue("");
    if (quote == '\'') {
      maybeValue = consumeAndExpandEntities<QuotedStringPredicate<'\''>,
                                            QuotedStringNoEntityPredicate<'\''>>();
    } else {
      maybeValue = consumeAndExpandEntities<QuotedStringPredicate<'"'>,
                                            QuotedStringNoEntityPredicate<'"'>>();
    }

    if (maybeValue.hasError()) {
      return std::move(maybeValue.error());
    }

    // Make sure that end quote is present
    if (!tryConsume(std::string_view(&quote, 1))) {
      if (quote == '\'') {
        return createParseError("Attribute value not closed with \"'\"");
      } else {
        return createParseError("Attribute value not closed with '\"'");
      }
    }

    ParsedAttribute result{name, maybeValue.result()};
    return std::make_optional(result);
  }

  // Parse XML attributes of the node
  [[nodiscard]] std::optional<ParseError> parseNodeAttributes(XMLNode& node) {
    // For all attributes
    while (true) {
      ParseResult<std::optional<ParsedAttribute>> maybeAttribute = parseNextAttribute();
      if (maybeAttribute.hasError()) {
        return std::move(maybeAttribute.error());
      }

      skipWhitespace();

      if (maybeAttribute.result().has_value()) {
        const ParsedAttribute& attribute = maybeAttribute.result().value();
        node.setAttribute(attribute.name, attribute.value);
      } else {
        break;
      }
    }

    // Skip whitespace after attributes.
    skipWhitespace();

    return std::nullopt;
  }
};

}  // namespace

ParseResult<XMLDocument> XMLParser::Parse(std::string_view str, const XMLParser::Options& options) {
  XMLParserImpl parser(str, options);
  return parser.parse();
}

std::optional<FileOffsetRange> XMLParser::GetAttributeLocation(
    std::string_view str, FileOffset elementStartOffset, const XMLQualifiedNameRef& attributeName) {
  if (!elementStartOffset.offset) {
    return std::nullopt;
  }

  XMLParser::Options reparseOptions;
  // To avoid unnecessary conversion when we're going to discard the values anyway.
  reparseOptions.disableEntityTranslation = true;

  const std::string_view elementToEnd = str.substr(elementStartOffset.offset.value());
  XMLParserImpl parser(elementToEnd, reparseOptions);
  if (auto attributeLocationInElement = parser.getElementAttributeLocation(attributeName)) {
    FileOffsetRange result{attributeLocationInElement->start.addParentOffset(elementStartOffset),
                           attributeLocationInElement->end.addParentOffset(elementStartOffset)};
    return result;
  }

  return std::nullopt;
}

}  // namespace donner::xml
