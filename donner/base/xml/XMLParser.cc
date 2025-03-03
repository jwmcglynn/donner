#include "donner/base/xml/XMLParser.h"

#include <cassert>  // For assert
#include <cstddef>
#include <cstdlib>  // For std::size_t
#include <iostream>
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
#include "donner/base/xml/components/EntityDeclarationsContext.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"

namespace donner::xml {

using donner::ParseError;
using donner::ParseResult;

namespace {

// LCOV_EXCL_START: Compile-time only
template <typename Lambda>
constexpr std::array<unsigned char, 256> BuildLookupTable(Lambda lambda) {
  std::array<unsigned char, 256> table = {};
  for (int i = 0; i < 256; ++i) {
    table[i] = lambda(static_cast<char>(i)) ? 1 : 0;
  }
  return table;
}
// LCOV_EXCL_STOP

struct ParsedAttribute {
  XMLQualifiedNameRef name;
  RcStringOrRef value;
};

/// Detects qualified name characters, e.g. element or attribute names, which may contain a colon
/// if they have a namespace prefix
struct NamePredicate {
  /// Valid names (anything but space `\n` `\r` `\t` `/` `<` `>` `=` `?` `!` `\0`)
  static constexpr std::array<unsigned char, 256> kLookupName = BuildLookupTable([](char ch) {
    return !(ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '/' || ch == '<' ||
             ch == '>' || ch == '=' || ch == '?' || ch == '!' || ch == '\0');
  });

  static unsigned char test(char ch) { return kLookupName[static_cast<unsigned char>(ch)]; }
};

/// Detects attribute name characters without ':', which may be a namespace prefix or local name
struct NameNoColonPredicate {
  /// Name without colon (anything but space `\n` `\r` `\t` `/` `<` `>` `=` `?` `!` `\0`
  /// `:`)
  static constexpr std::array<unsigned char, 256> kLookupNameNoColon =
      BuildLookupTable([](char ch) {
        return !(ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '/' || ch == '<' ||
                 ch == '>' || ch == '=' || ch == '?' || ch == '!' || ch == '\0' || ch == ':');
      });

  static unsigned char test(char ch) { return kLookupNameNoColon[static_cast<unsigned char>(ch)]; }
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

/// Detects text data within nodes, e.g. between <tag> and </tag> which does not require
/// reprocessing (anything but `<` `\0` `%`)
struct TextNoParameterEntityPredicate {
  /// Text (i.e. PCDATA) that does not require reprocessing (anything but `<` `\0` `%`)
  static constexpr std::array<unsigned char, 256> kLookupTextNoEntity =
      BuildLookupTable([](char ch) { return ch != '<' && ch != '\0' && ch != '%'; });

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

/// Append a codepoint as a new string to the pieces vector.
std::optional<ParseError> AppendUnicodeCharToNewString(char32_t codepoint,
                                                       SmallVector<RcStringOrRef, 5>& pieces,
                                                       size_t offset) {
  // Validate the codepoint per XML specs.
  if (!Utf8::IsValidCodepoint(codepoint) || codepoint == 0xFFFE || codepoint == 0xFFFF) {
    ParseError err;
    err.reason = "Invalid numeric character entity";
    err.location = FileOffset::Offset(offset);
    return err;
  }

  // Allocate a new string, append UTF-8, and record its view.
  std::vector<char> str;
  Utf8::Append(codepoint, std::back_inserter(str));
  pieces.push_back(RcStringOrRef(RcString::fromVector(std::move(str))));
  return std::nullopt;
}

/**
 * A helper class to encapsulate entity expansion.
 * It takes a starting std::string_view and then parses built-in and numeric entities.
 * Custom entity lookups could be added via additional callbacks if needed.
 */
class EntityExpander {
public:
  enum class Type {
    General,    ///< General entity expansion, e.g. '&amp;'
    Parameter,  ///< Parameter entity expansion, e.g. '%foo;', for use in the DTD.
  };

  EntityExpander(Type type, std::string_view input, components::EntityDeclarationsContext& context,
                 int depth = 0)
      : type_(type), input_(input), originalInput_(input), context_(context), entityDepth_(depth) {}

  /**
   * Expand the input text, returning a SmallVector of string_view pieces.
   */
  ParseResult<SmallVector<RcStringOrRef, 5>> expand() {
    using parser::IntegerParser;

    SmallVector<RcStringOrRef, 5> pieces;
    const std::string_view symbol = type_ == Type::General ? "&" : "%";

    // Process the rest of the input.
    while (!input_.empty() && TextPredicate::test(peek().value_or('\0'))) {
      if (peek() != symbol[0]) {
        // Consume a chunk of characters that don't need further processing.
        std::string_view chunk;

        if (type_ == Type::General) {
          chunk = consumeMatching<TextNoEntityPredicate>();
        } else {
          chunk = consumeMatching<TextNoParameterEntityPredicate>();
        }

        if (!chunk.empty()) {
          pieces.push_back(RcStringOrRef(chunk));
        }
      } else {
        // Process an entity.
        if (tryConsume("&amp;")) {
          pieces.push_back("&");
        } else if (tryConsume("&apos;")) {
          pieces.push_back("'");
        } else if (tryConsume("&quot;")) {
          pieces.push_back("\"");
        } else if (tryConsume("&lt;")) {
          pieces.push_back("<");
        } else if (tryConsume("&gt;")) {
          pieces.push_back(">");
        } else if (tryConsume("&#")) {
          const size_t entityOffset = originalInput_.size() - input_.size();

          // Numeric entity: hex or decimal.
          if (peek() == 'x') {
            // Hexadecimal.
            auto maybeHex = IntegerParser::ParseHex(input_.substr(1));
            if (maybeHex.hasError()) {
              ParseError err = maybeHex.error();
              err.location = err.location.addParentOffset(FileOffset::Offset(entityOffset + 1));
              return err;
            }

            const IntegerParser::Result result = maybeHex.result();
            input_.remove_prefix(result.consumedChars + 1);

            if (auto err = AppendUnicodeCharToNewString(result.number, pieces, entityOffset + 1)) {
              err->location = err->location.addParentOffset(FileOffset::Offset(entityOffset));
              return err.value();
            }
          } else {
            // Decimal.
            auto maybeDec = IntegerParser::Parse(input_);
            if (maybeDec.hasError()) {
              ParseError err = maybeDec.error();
              err.location = err.location.addParentOffset(FileOffset::Offset(entityOffset));
              return err;
            }

            const IntegerParser::Result result = maybeDec.result();
            input_.remove_prefix(result.consumedChars);

            if (auto err = AppendUnicodeCharToNewString(result.number, pieces, entityOffset)) {
              err->location = err->location.addParentOffset(FileOffset::Offset(entityOffset));
              return err.value();
            }
          }

          if (!tryConsume(";")) {
            return ParseError{"Numeric character entity missing closing ';'"};
          }
        } else {
          // Custom entity
          std::string_view inputFromEntityStart = input_;
          input_.remove_prefix(1);  // skip '&' or '%'
          size_t namePos = 0;
          while (namePos < input_.size() && NameNoColonPredicate::test(input_[namePos]) &&
                 input_[namePos] != ';') {
            ++namePos;
          }

          if (namePos == 0 || namePos >= input_.size()) {
            pieces.push_back(symbol);
            continue;
          }

          if (input_[namePos] == ';') {
            std::string_view entityName = input_.substr(0, namePos);
            input_.remove_prefix(namePos + 1);

            if (auto entityDecl =
                    (type_ == Type::General ? context_.getEntityDeclaration(entityName)
                                            : context_.getParameterEntityDeclaration(entityName))) {
              if (entityDecl->second) {
                std::cout << "chunk ext: " << inputFromEntityStart.substr(0, namePos + 2)
                          << std::endl;

                // External entities are not yet supported
                // TODO: Implement external entity resolution.
                pieces.push_back(RcStringOrRef(inputFromEntityStart.substr(0, namePos + 2)));
                continue;
              } else {
                // Recursive entity expansion.
                if (entityDepth_ >= kMaxEntityDepth) {
                  // Prevent infinite recursion by using the literal entity text.
                  pieces.push_back(RcStringOrRef(inputFromEntityStart.substr(0, namePos + 2)));
                  continue;
                }

                EntityExpander recursiveExpander(EntityExpander::Type::General, entityDecl->first,
                                                 context_, entityDepth_ + 1);
                auto recursiveResult = recursiveExpander.expand();
                if (recursiveResult.hasError()) {
                  return recursiveResult.error();
                }

                for (const auto& piece : recursiveResult.result()) {
                  // Ensure we save by-value into an allocated RcString, as the string returned by
                  // getEntityDeclaration may be on the stack.
                  pieces.push_back(RcStringOrRef(RcString(piece)));
                }
              }
            } else {
              pieces.push_back(inputFromEntityStart.substr(0, namePos + 2));
            }
          } else {
            pieces.push_back(symbol);
          }
        }
      }
    }

    return pieces;
  }

private:
  inline std::optional<char> peek() const {
    if (input_.empty()) {
      return std::nullopt;
    }
    return input_[0];
  }

  inline bool tryConsume(std::string_view token) {
    if (input_.starts_with(token)) {
      input_.remove_prefix(token.size());
      return true;
    }
    return false;
  }

  template <class Predicate>
  inline std::string_view consumeMatching() {
    size_t i = 0;
    while (i < input_.size() && Predicate::test(input_[i])) {
      ++i;
    }
    std::string_view result = input_.substr(0, i);
    input_.remove_prefix(i);
    return result;
  }

  Type type_;

  std::string_view input_;
  std::string_view originalInput_;

  components::EntityDeclarationsContext& context_;

  // Recursion tracking to avoid infinite entity expansions.
  static constexpr int kMaxEntityDepth = 10;
  int entityDepth_;
};

class XMLParserImpl : public parser::ParserBase {
private:
  XMLDocument document_;
  XMLParser::Options options_;
  std::optional<parser::LineOffsets> lineOffsets_;

public:
  explicit XMLParserImpl(std::string_view text, const XMLParser::Options& options)
      : parser::ParserBase(text), options_(options) {
    document_.registry().ctx().emplace<components::EntityDeclarationsContext>();
  }

  ParseResult<XMLDocument> parse() {
    if (str_.empty()) {
      return document_;
    }

    assert(!remaining_.empty() && "parse() already called");

    // Detect and skip the BOM, if it exists
    parseBOM();

    // Parse top-level nodes
    while (true) {
      skipWhitespace();
      if (remaining_.empty() || peek() == '\0') {
        break;
      }

      const FileOffset startOffset = currentOffsetWithLineNumber();
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

  /**
   * Used by GetAttributeLocation to re-parse just the attributes of a single element
   * starting at `<element`.
   */
  std::optional<FileOffsetRange> getElementAttributeLocation(const XMLQualifiedNameRef& name) {
    // We assume the caller has already consumed "<", so do it here.
    UTILS_RELEASE_ASSERT_MSG(tryConsume("<"), "Expected element to start with '<'");

    // Extract element name
    auto maybeName = consumeQualifiedName();
    UTILS_RELEASE_ASSERT_MSG(!maybeName.hasError(),
                             "Expected element to have previously parsed correctly");

    // Skip whitespace between element name and attributes
    skipWhitespace();

    // Now parse attributes until we reach `>` or `/>` or we run out
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
      lineOffsets_.emplace(str_);
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

  /// Consume and return a substring while a predicate evaluates to true
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

  /// The main expansion routine for text / attribute values. Handles built-ins (`&amp;` etc.),
  /// numeric references (`&#x12;`), and custom entities from `<!ENTITY>` declarations.
  template <class MatchPredicate, class MatchPredicateNoEntity>
  ParseResult<RcStringOrRef> consumeAndExpandEntities() {
    // Fast path if translation is disabled.
    if (UTILS_PREDICT_FALSE(options_.disableEntityTranslation)) {
      // Just read raw text until the first disallowed character
      return RcStringOrRef(consumeMatching<MatchPredicate>());
    }

    // First pass: read raw text that doesn't need reprocessing
    const std::string_view unmodifiedContents = consumeMatching<MatchPredicateNoEntity>();

    // If the entire string was unmodified, return it. Otherwise we need to do entity conversion.
    if (!MatchPredicate::test(peek().value_or('\0'))) {
      return RcStringOrRef(unmodifiedContents);
    }

    SmallVector<RcStringOrRef, 5> pieces;
    if (!unmodifiedContents.empty()) {
      pieces.push_back(RcStringOrRef(unmodifiedContents));
    }

    // Read the rest of the input.
    std::string_view entityContainingContents;
    const FileOffset entityStartLocation = currentOffsetWithLineNumber();
    {
      size_t matchingLength = 0;
      while (matchingLength < remaining_.size() &&
             MatchPredicate::test(remaining_[matchingLength])) {
        ++matchingLength;
      }

      entityContainingContents = remaining_.substr(0, matchingLength);
      remaining_.remove_prefix(matchingLength);
    }

    // Instantiate the expander with the current remaining text.
    EntityExpander expander(
        EntityExpander::Type::General, entityContainingContents,
        document_.registry().ctx().get<components::EntityDeclarationsContext>());
    auto maybePieces = expander.expand();
    if (maybePieces.hasError()) {
      ParseError outerError;
      outerError.reason = maybePieces.error().reason;
      outerError.location = maybePieces.error().location.addParentOffset(entityStartLocation);
      return outerError;
    }

    for (auto& piece : maybePieces.result()) {
      pieces.push_back(std::move(piece));
    }

    if (pieces.size() == 1) {
      // Make sure its allocated.
      return RcStringOrRef(RcString(pieces[0]));
    }

    // Flatten all the pieces.
    size_t totalLen = 0;
    for (const auto& sv : pieces) {
      totalLen += sv.size();
    }

    std::vector<char> buffer;
    buffer.reserve(totalLen);
    for (const auto& sv : pieces) {
      buffer.insert(buffer.end(), sv.begin(), sv.end());
    }

    return RcStringOrRef(RcString::fromVector(std::move(buffer)));
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

  /// Parse XML declaration (<?xml...)
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

  /**
   * Parse DOCTYPE, e.g. `<!DOCTYPE root [ ... ]>`
   *
   * We store the entire doctype text in the node’s value(), but also
   * detect `<!ENTITY>` declarations in the internal subset and record them.
   */
  ParseResult<std::optional<XMLNode>> parseDoctype(FileOffset startOffset) {
    // We read until the first '>' at nesting level 0, while also handling the internal subset `[
    // ]`
    int bracketLevel = 0;
    bool foundEnd = false;
    bool inInternalSubset = false;

    size_t i = 0;
    while (i < remaining_.size()) {
      char c = remaining_[i];
      if (c == '\0') {
        return createParseError("Unexpected end of data, found embedded null character");
      }
      if (c == '[') {
        bracketLevel++;
        inInternalSubset = true;
      } else if (c == ']') {
        bracketLevel--;
        if (bracketLevel < 0) {
          bracketLevel = 0;  // Malformed but we won't crash
        }
        if (bracketLevel == 0) {
          inInternalSubset = false;
        }
      } else if (c == '>' && bracketLevel == 0) {
        // Doctype ends here
        foundEnd = true;
        break;
      } else if (inInternalSubset && i + 8 < remaining_.size() &&
                 remaining_.substr(i, 8) == "<!ENTITY") {
        // Parse entity declaration, find the '>' that ends this !ENTITY
        size_t closePos = remaining_.find('>', i + 8);
        if (closePos == std::string_view::npos) {
          return createParseError("Unterminated <!ENTITY declaration in DOCTYPE");
        }
        // Substring for the entity decl
        std::string_view entityDecl = remaining_.substr(i, closePos - i + 1);
        // Store it
        if (auto maybeErr = parseEntityDeclInDoctype(entityDecl)) {
          return maybeErr.value();
        }
        // Skip over
        i = closePos;  // The for loop will i++ after
      }
      i++;
    }

    if (!foundEnd) {
      return createParseError("Doctype node missing closing '>'");
    }

    // The substring includes everything up to `i`
    std::string_view doctypeStr = remaining_.substr(0, i);
    remaining_.remove_prefix(i + 1);  // consume the '>' as well

    if (options_.parseDoctype) {
      XMLNode docNode = XMLNode::CreateDocTypeNode(document_, doctypeStr);
      docNode.setSourceStartOffset(startOffset);
      docNode.setSourceEndOffset(currentOffsetWithLineNumber());
      return std::optional<XMLNode>(docNode);
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

  /**
   * Read raw text (PCDATA) until `<` or `\0`
   */
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

  /**
   * parseCData: e.g. `<![CDATA[ ... ]]>`
   */
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

  /**
   * parseEntityDeclInDoctype: given a snippet like `<!ENTITY name "value">`
   * or `<!ENTITY % name "value">` or with SYSTEM, store it in the entity registry.
   *
   * We do not fully expand parameter entities inside the entity value except for
   * what your test suite already covers. If you need more advanced expansions, you
   * can unify this with a more thorough parser approach.
   */
  std::optional<ParseError> parseEntityDeclInDoctype(std::string_view decl) {
    // quick sanity check
    // The string starts with `<!ENTITY` ... ends with '>'
    // strip that off
    static constexpr std::string_view kPrefix = "<!ENTITY";
    if (!decl.starts_with(kPrefix)) {
      return createParseError("Expected <!ENTITY in parseEntityDeclInDoctype");
    }
    // remove `<!ENTITY`
    decl.remove_prefix(kPrefix.size());

    // remove trailing '>'
    if (!decl.ends_with(">")) {
      return createParseError("Missing '>' in entity declaration");
    }
    decl.remove_suffix(1);

    // skip whitespace
    size_t pos = 0;
    while (pos < decl.size() && isWhitespace(decl[pos])) {
      pos++;
    }

    bool isParameterEntity = false;
    if (pos < decl.size() && decl[pos] == '%') {
      // parameter entity
      isParameterEntity = true;
      pos++;
      while (pos < decl.size() && isWhitespace(decl[pos])) {
        pos++;
      }
    }

    // parse entity name
    size_t nameStart = pos;
    while (pos < decl.size() && NameNoColonPredicate::test(decl[pos])) {
      pos++;
    }
    if (pos == nameStart) {
      return createParseError("Expected entity name");
    }
    RcString entityName(decl.substr(nameStart, pos - nameStart));

    // skip whitespace
    while (pos < decl.size() && isWhitespace(decl[pos])) {
      pos++;
    }
    if (pos >= decl.size()) {
      return createParseError("Entity declaration truncated");
    }

    bool isExternal = false;
    RcString entityValue;

    // Check if "SYSTEM" or "PUBLIC"
    if ((pos + 6 <= decl.size()) &&
        (decl.compare(pos, 6, "SYSTEM") == 0 || decl.compare(pos, 6, "PUBLIC") == 0)) {
      // external
      isExternal = true;
      pos += 6;
      while (pos < decl.size() && isWhitespace(decl[pos])) {
        pos++;
      }
      if (pos >= decl.size()) {
        return createParseError("Truncated external entity decl");
      }
      if (decl[pos] == '"' || decl[pos] == '\'') {
        // parse system identifier
        char quote = decl[pos++];
        size_t valStart = pos;
        while (pos < decl.size() && decl[pos] != quote) {
          pos++;
        }
        if (pos >= decl.size()) {
          return createParseError("Unterminated external entity system identifier");
        }
        entityValue = RcString(decl.substr(valStart, pos - valStart));
        pos++;  // skip closing quote
      } else {
        return createParseError("Expected quoted system identifier in entity decl");
      }
    } else {
      // internal entity => must be quoted
      if (decl[pos] != '"' && decl[pos] != '\'') {
        return createParseError("Expected quoted entity value or SYSTEM/PUBLIC");
      }
      char quote = decl[pos++];
      size_t valStart = pos;
      while (pos < decl.size() && decl[pos] != quote) {
        pos++;
      }
      if (pos >= decl.size()) {
        return createParseError("Unterminated entity value");
      }
      entityValue = RcString(decl.substr(valStart, pos - valStart));
      pos++;  // skip closing quote
    }

    // Resolve parameter entity references
    EntityExpander expander(
        EntityExpander::Type::Parameter, entityValue,
        document_.registry().ctx().get<components::EntityDeclarationsContext>());
    auto maybePieces = expander.expand();
    if (maybePieces.hasError()) {
      return maybePieces.error();
    }

    // Reassemble the entity value
    const SmallVector<RcStringOrRef, 5>& pieces = maybePieces.result();

    RcString expandedEntityValue;
    if (pieces.size() == 1) {
      // If there's only one piece, we can just use it directly.
      expandedEntityValue = RcString(pieces[0]);
    } else {
      std::vector<char> entityValueBuffer;
      for (const auto& piece : maybePieces.result()) {
        entityValueBuffer.insert(entityValueBuffer.end(), piece.begin(), piece.end());
      }

      expandedEntityValue = RcString::fromVector(std::move(entityValueBuffer));
    }

    // store in the entity declarations
    auto& entityCtx = document_.registry().ctx().get<components::EntityDeclarationsContext>();
    if (isParameterEntity) {
      entityCtx.addParameterEntityDeclaration(entityName, expandedEntityValue, isExternal);
    } else {
      entityCtx.addEntityDeclaration(entityName, expandedEntityValue, isExternal);
    }
    return std::nullopt;
  }

  /**
   * Parsing an element of form `<tag ...>` or `<tag .../>`.
   */
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

  /**
   * Parse a node, dispatch on what comes after `<`
   */
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

  /**
   * Parse contents of the node, gather child nodes or text until `</tag>`
   */
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

  /**
   * Attempt to parse a single attr `name="value"`
   *
   * @returns nullopt if none found.
   */
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

  /**
   * Parse XML attributes of the node, gather all attributes until `>` or `/>`
   */
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

ParseResult<XMLDocument> XMLParser::Parse(std::string_view str, const Options& options) {
  XMLParserImpl parser(str, options);
  return parser.parse();
}

std::optional<FileOffsetRange> XMLParser::GetAttributeLocation(
    std::string_view str, FileOffset elementStartOffset, const XMLQualifiedNameRef& attributeName) {
  if (!elementStartOffset.offset) {
    return std::nullopt;
  }

  Options reparseOptions;
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
