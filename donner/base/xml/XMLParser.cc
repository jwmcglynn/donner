#include "donner/base/xml/XMLParser.h"

#include <cassert>  // For assert
#include <cstddef>
#include <cstdlib>  // For std::size_t
#include <string_view>

#include "donner/base/ChunkedString.h"
#include "donner/base/FileOffset.h"
#include "donner/base/MathUtils.h"
#include "donner/base/ParseError.h"
#include "donner/base/RcString.h"
#include "donner/base/RcStringOrRef.h"
#include "donner/base/Utf8.h"
#include "donner/base/Utils.h"
#include "donner/base/parser/IntegerParser.h"
#include "donner/base/parser/LineOffsets.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/EntityDeclarationsContext.h"

namespace donner::xml {

using donner::ParseError;
using donner::ParseResult;

namespace {

/// The maximum length for a string's entity resolution, to prevent "fork bomb" style attacks like
/// "Billion Laughs".
/// @see https://en.wikipedia.org/wiki/Billion_laughs_attack
static constexpr size_t kMaxEntityResolutionLength = 1024 * 64;  // 64KB

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

/// Finds the '>' that ends <!ENTITY, skipping any '>' inside quoted text.
/// Returns std::string_view::npos if none found.
static size_t FindEntityDeclEnd(const ChunkedString& text, size_t start) {
  // We assume text.substr(start) begins after "<!ENTITY".
  // We'll track whether we are in a single-quote or double-quote section.
  bool inSingleQuote = false;
  bool inDoubleQuote = false;

  for (size_t pos = start; pos < text.size(); ++pos) {
    const char c = text[pos];
    if (c == '\0') {
      // Defensive check for null char
      break;
    }
    if (!inSingleQuote && !inDoubleQuote) {
      // Not in quotes:
      if (c == '\'') {
        inSingleQuote = true;
      } else if (c == '"') {
        inDoubleQuote = true;
      } else if (c == '>') {
        return pos;
      }
    } else if (inSingleQuote) {
      // Inside single quotes
      if (c == '\'') {
        inSingleQuote = false;
      }
    } else {
      // Inside double quotes
      if (c == '"') {
        inDoubleQuote = false;
      }
    }
  }
  return std::string_view::npos;
}

struct ParsedAttribute {
  XMLQualifiedNameRef name;
  RcStringOrRef value;
};

/// Detects qualified name characters, e.g. element or attribute names, which may contain a colon
/// if they have a namespace prefix
struct NamePredicate {
  /// Valid names (anything but space `\n` `\r` `\t` `/` `<` `>` `=` `?` `!` `\0` `'` `"`)
  static constexpr std::array<unsigned char, 256> kLookupName = BuildLookupTable([](char ch) {
    return !(ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '/' || ch == '<' ||
             ch == '>' || ch == '=' || ch == '?' || ch == '!' || ch == '\0' || ch == '\'' ||
             ch == '"');
  });

  static unsigned char test(char ch) { return kLookupName[static_cast<unsigned char>(ch)]; }
};

/// Detects digits for numeric entities (0-9, a-f, A-F)
struct DigitsPredicate {
  /// Digits (0-9, a-f, A-F for hex values)
  static constexpr std::array<unsigned char, 256> kLookupDigits = BuildLookupTable([](char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
  });

  static unsigned char test(char ch) { return kLookupDigits[static_cast<unsigned char>(ch)]; }
};

/// Detects attribute name characters without ':', which may be a namespace prefix or local name
struct NameNoColonPredicate {
  /// Name without colon (anything but space `\n` `\r` `\t` `/` `<` `>` `=` `?` `!` `\0`
  /// `:`, `'`, `"`)
  static constexpr std::array<unsigned char, 256> kLookupNameNoColon =
      BuildLookupTable([](char ch) {
        return !(ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '/' || ch == '<' ||
                 ch == '>' || ch == '=' || ch == '?' || ch == '!' || ch == '\0' || ch == ':' ||
                 ch == '\'' || ch == '"');
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

/// Matches characters except `\0`.
struct AnyPredicate {
  static bool test(char ch) { return ch != '\0'; }
};

/// Matches characters except parameter entity references (e.g. `%amp;`), any character except `%`
/// and `\0`.
struct NoParameterEntityPredicate {
  /// Quoted string contents which does not require processing (anything but `%` and `\0`)
  static constexpr std::array<unsigned char, 256> kLookupNoEntity =
      BuildLookupTable([](char ch) { return ch != '%' && ch != '\0'; });

  static bool test(char ch) { return kLookupNoEntity[static_cast<unsigned char>(ch)] != 0; }
};

/// Append a codepoint as a new string to the pieces vector.
std::optional<ParseError> AppendUnicodeCharToNewString(char32_t codepoint,
                                                       ChunkedString& chunkedString,
                                                       size_t offset) {
  // Validate the codepoint per XML specs.
  if (!Utf8::IsValidCodepoint(codepoint) || codepoint == 0xFFFE || codepoint == 0xFFFF) {
    ParseError err;
    err.reason = "Invalid numeric character entity";
    err.location = FileOffset::Offset(offset);
    return err;
  }

  // Allocate a new string, append UTF-8, and record its view.
  std::string str;
  Utf8::Append(codepoint, std::back_inserter(str));
  chunkedString.append(RcString(str));
  return std::nullopt;
}

class XMLParserImpl {
private:
  XMLDocument document_;
  components::EntityDeclarationsContext& entityCtx_;

  /// The original string.
  const std::string_view str_;

  /// Remaining characters from \ref str_, potentially modified for entity resolution.
  ChunkedString remaining_;

  XMLParser::Options options_;
  std::optional<parser::LineOffsets> lineOffsets_;

  const int maxEntityDepth_;
  const uint64_t maxEntitySubstitutions_;
  uint64_t entitySubstitutionCount_ = 0;

public:
  explicit XMLParserImpl(std::string_view text, const XMLParser::Options& options)
      : entityCtx_(document_.registry().ctx().emplace<components::EntityDeclarationsContext>()),
        str_(text),
        remaining_(text),
        options_(options),
        maxEntityDepth_(options.maxEntityDepth),
        maxEntitySubstitutions_(options.maxEntitySubstitutions) {}

  bool isWhitespace(char ch) const {
    // Whitespace is defined by multiple specs, but both match.
    //
    // - https://www.w3.org/TR/css-transforms-1/#svg-wsp
    //   Either a U+000A LINE FEED, U+000D CARRIAGE RETURN, U+0009 CHARACTER TABULATION, or U+0020
    //   SPACE.
    //
    // - https://www.w3.org/TR/xml/#NT-S
    //   S (white space) consists of one or more space (#x20) characters, carriage returns, line
    //   feeds, or tabs.
    //   S ::= (#x20 | #x9 | #xD | #xA)+

    return ch == '\t' || ch == ' ' || ch == '\n' || ch == '\r';
  }

  FileOffset currentOffset(ChunkedString& sourceString, int index) const {
    if (sourceString.empty()) {
      return FileOffset::Offset(str_.size());
    }

    // Attempt to grab a single-character substring from the provided sourceString.
    // This won’t perform any new allocation; it’s just a slice.
    ChunkedString oneChar = sourceString.substr(index, 1);

    // Convert that substring to a string_view.
    std::string_view oneCharView = oneChar.firstChunk();
    assert(!oneCharView.empty());

    // Check that the pointer is within [str_.data(), str_.data() + str_.size()).
    // If not, fallback to end of string.
    if (oneCharView.data() >= str_.data() && oneCharView.data() < str_.data() + str_.size()) {
      const size_t offset = static_cast<size_t>(oneCharView.data() - str_.data());
      return FileOffset::Offset(offset);

    } else {
      // Not actually pointing back into our original `str_`.
      return FileOffset::EndOfString();
    }
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
      skipWhitespace(remaining_);
      if (remaining_.empty() || peek(remaining_) == '\0') {
        break;
      }

      const FileOffset startOffset = currentOffsetWithLineNumber(remaining_);

      if (tryConsume(remaining_, "<")) {
        auto maybeNode = parseNode(startOffset);
        if (maybeNode.hasError()) {
          return std::move(maybeNode.error());
        }
        if (maybeNode.result().has_value()) {
          document_.root().appendChild(maybeNode.result().value());
        }
      } else {
        // Try to parse PCData, but only accept if the first result is a node.
        auto maybeData = consumePCDataOnce();
        if (maybeData.hasError()) {
          return std::move(maybeData.error());
        }

        if (!maybeData.result().empty()) {
          return createParseError("Expected '<' to start a node", startOffset);
        }

        // Try again to parse a node.
        continue;
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
    UTILS_RELEASE_ASSERT_MSG(tryConsume(remaining_, "<"), "Expected element to start with '<'");

    // Extract element name
    auto maybeName = consumeQualifiedName();
    UTILS_RELEASE_ASSERT_MSG(!maybeName.hasError(),
                             "Expected element to have previously parsed correctly");

    // Skip whitespace between element name and attributes
    skipWhitespace(remaining_);

    // Now parse attributes until we reach `>` or `/>` or we run out
    while (true) {
      const FileOffset attributeStartOffset = currentOffsetWithLineNumber(remaining_);

      ParseResult<std::optional<ParsedAttribute>> maybeAttribute = parseNextAttribute();
      UTILS_RELEASE_ASSERT_MSG(!maybeAttribute.hasError(),
                               "Expected element to have previously parsed correctly");

      const FileOffset attributeEndOffset = currentOffsetWithLineNumber(remaining_);
      skipWhitespace(remaining_);

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

  FileOffset currentOffsetWithLineNumber(ChunkedString& sourceString, int relativeOffset = 0) {
    FileOffset current = currentOffset(sourceString, relativeOffset);
    if (!current.offset) {
      return current;
    }

    const size_t offset = current.offset.value();
    return lineOffsets().fileOffset(offset);
  }

  ParseError createParseError(std::string_view reason,
                              std::optional<FileOffset> location = std::nullopt) {
    ParseError result;
    result.reason = reason;
    result.location = location.value_or(currentOffsetWithLineNumber(remaining_));
    return result;
  }

  [[nodiscard]] std::optional<ParseError> recordEntitySubstitution(const FileOffset& entityOffset) {
    if (entitySubstitutionCount_ >= maxEntitySubstitutions_) {
      return createParseError("Entity substitution limit exceeded", entityOffset);
    }

    ++entitySubstitutionCount_;
    return std::nullopt;
  }

  /**
   * Insert a decoded unicode character, an integer in the range [0, 0x10FFFF], and inserts Utf8
   * codepoints into the \c it output iterator.
   *
   * @param ch Unicode character to insert, in the range [0, 0x10FFFF].
   * @param it Output iterator to insert UTF-8 codepoints.
   * @return An error if the character is invalid, or \c std::nullopt if the character was
   * inserted
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

  /// Skip whitespace characters
  void skipWhitespace(ChunkedString& sourceString) {
    size_t skipCount = 0;
    const size_t len = sourceString.size();

    while (skipCount < len && isWhitespace(sourceString[skipCount])) {
      ++skipCount;
    }

    sourceString.remove_prefix(skipCount);
  }

  /// Consume and return a substring while a predicate evaluates to true
  template <class MatchPredicate>
  static ChunkedString consumeMatching(ChunkedString& sourceString) {
    size_t i = 0;
    const size_t len = sourceString.size();

    while (i < len && MatchPredicate::test(sourceString[i])) {
      ++i;
    }

    const ChunkedString result = sourceString.substr(0, i);
    sourceString.remove_prefix(i);
    return result;
  }

  /**
   * Attempt to parse a built-in or numeric entity. If we successfully parse one, we append the
   * decoded text (e.g. "<") to `out` and return true. Otherwise, we return false.
   *
   * @param sourceString The input string to parse.
   * @param out The output string to append the decoded text to.
   * @return true if we successfully parsed a built-in or numeric entity, false otherwise.
   */
  ParseResult<bool> tryParseBuiltInOrNumericEntity(const FileOffset& entityOffset,
                                                   ChunkedString& sourceString,
                                                   ChunkedString& out) {
    using std::string_view_literals::operator""sv;
    using parser::IntegerParser;

    // Try built-in first
    if (tryConsume(sourceString, "&amp;")) {
      out.append("&"sv);
      return true;
    } else if (tryConsume(sourceString, "&apos;")) {
      out.append("'"sv);
      return true;
    } else if (tryConsume(sourceString, "&quot;")) {
      out.append("\""sv);
      return true;
    } else if (tryConsume(sourceString, "&lt;")) {
      out.append("<"sv);
      return true;
    } else if (tryConsume(sourceString, "&gt;")) {
      out.append(">"sv);
      return true;
    }
    // Then numeric entities: '&#' prefix
    else if (tryConsume(sourceString, "&#")) {
      const bool hex = tryConsume(sourceString, "x");
      const FileOffset digitsOffset = currentOffsetWithLineNumber(sourceString);

      // Grab all digits
      const RcString digits = consumeMatching<DigitsPredicate>(sourceString).toSingleRcString();
      if (digits.empty()) {
        ParseError err;
        err.reason = "Invalid numeric entity syntax (missing digits)";
        err.location = entityOffset;
        return err;
      }

      auto parseRes = hex ? IntegerParser::ParseHex(digits) : IntegerParser::Parse(digits);
      if (parseRes.hasError()) {
        ParseError err = parseRes.error();
        if (digitsOffset.offset) {
          err.location = err.location.addParentOffset(digitsOffset);
        } else {
          // For recursive entity expansions, source information is lost.
          // TODO: Find a way to retain this information.
          err.location.offset = std::nullopt;
        }
        return err;
      }

      const size_t codepoint = parseRes.result().number;

      // We must see a trailing ';'
      if (!tryConsume(sourceString, ";")) {
        ParseError semicolonErr;
        semicolonErr.reason = "Numeric character entity missing closing ';'";
        semicolonErr.location = currentOffsetWithLineNumber(sourceString);
        return semicolonErr;
      }

      // Validate and append
      if (auto maybeErr =
              AppendUnicodeCharToNewString(codepoint, out, entityOffset.offset.value_or(0))) {
        return maybeErr.value();
      }

      return true;
    }

    return false;  // Not a built-in or numeric entity
  }

  template <typename MatchPredicate, typename MatchPredicateNoEntity>
  ParseResult<ChunkedString> consumeAndExpandEntities(components::EntityType type,
                                                      ChunkedString& sourceString) {
    using std::string_view_literals::operator""sv;

    // Fast path if translation is disabled.
    if (UTILS_PREDICT_FALSE(options_.disableEntityTranslation)) {
      // Just read raw text until the first disallowed character
      return consumeMatching<MatchPredicate>(sourceString);
    }

    const std::string_view kEntityPrefix = type == components::EntityType::General ? "&"sv : "%"sv;
    ChunkedString decodedText;

    size_t previousPrependRemaining = 0;
    int depth = 0;

    while (!sourceString.empty()) {
      // 1. Read as much raw text as possible until (no '&'/'%' or quote)
      {
        ChunkedString rawChunk = consumeMatching<MatchPredicateNoEntity>(sourceString);
        if (!rawChunk.empty()) {
          decodedText.append(rawChunk);
          previousPrependRemaining -= Min(previousPrependRemaining, rawChunk.size());
        }
      }

      // If we're at end-of-input or the predicate no longer matches (either a '<' or quote),
      // nothing more to decode
      const std::optional<char> nextChar = peek(sourceString);

      if (!nextChar || !MatchPredicate::test(nextChar.value())) {
        break;
      }

      // Otherwise, next char must be the expected entity prefix, '&' or '%'.
      assert(nextChar == kEntityPrefix[0]);

      const FileOffset entityOffset = currentOffsetWithLineNumber(sourceString);

      // 2. Try built-in or numeric
      auto parseResult = tryParseBuiltInOrNumericEntity(entityOffset, sourceString, decodedText);
      if (parseResult.hasError()) {
        return parseResult.error();
      }

      if (parseResult.result()) {
        if (auto maybeErr = recordEntitySubstitution(entityOffset)) {
          return maybeErr.value();
        }

        // We consumed a built-in or numeric => success, loop again
        continue;
      }

      // 3. If it's not built-in or numeric => custom entity => expand
      {
        const bool nameStartIndex = 1;  // Index 0 is the entity prefix, '&' or '%'
        size_t entPos = nameStartIndex;
        while (entPos < sourceString.size() && NameNoColonPredicate::test(sourceString[entPos]) &&
               sourceString[entPos] != ';') {
          entPos++;
        }

        if (entPos >= sourceString.size() || sourceString[entPos] != ';') {
          // Not well-formed => treat '&' literally
          sourceString.remove_prefix(1);  // Skip the entity prefix, '&' or '%'
          previousPrependRemaining -= Min(previousPrependRemaining, size_t(1));
          decodedText.append(kEntityPrefix);
          continue;
        } else {
          const ChunkedString entityNameChunk = sourceString.substr(1, entPos - 1);
          const RcString entityNameStr = entityNameChunk.toSingleRcString();

          if (auto decl = entityCtx_.getEntityDeclaration(type, entityNameStr)) {
            if (!decl->second) {
              // A known custom entity => expand

              int newDepth = depth;
              if (previousPrependRemaining) {
                ++newDepth;
              } else if (newDepth > 0) {
                --newDepth;
              }

              if (newDepth >= maxEntityDepth_) {
                decodedText.append(sourceString.substr(0, entPos + 1));
                sourceString.remove_prefix(entPos + 1);
                previousPrependRemaining -= Min(previousPrependRemaining, entPos + 1);
                continue;
              }

              if (auto maybeErr = recordEntitySubstitution(entityOffset)) {
                return maybeErr.value();
              }

              depth = newDepth;

              const size_t newTotalSize =
                  decodedText.size() + decl->first.size() + sourceString.size() - entPos - 1;

              if (newTotalSize >= kMaxEntityResolutionLength) {
                // Detect too long => literal
                decodedText.append(sourceString.substr(0, entPos + 1));
                sourceString.remove_prefix(entPos + 1);  // Remove '&name;' or '%name;'
                continue;
              }

              sourceString.remove_prefix(entPos + 1);  // Remove '&name;' or '%name;'
              sourceString.prepend(decl->first);       // If e.g. <rect />
              previousPrependRemaining = decl->first.size();

            } else {
              // External entity => not supported => literal
              decodedText.append(sourceString.substr(0, entPos + 1));
              sourceString.remove_prefix(entPos + 1);  // Remove '&name;' or '%name'
              previousPrependRemaining -= Min(previousPrependRemaining, entPos + 1);
            }
          } else {
            // Unknown => literal
            decodedText.append(sourceString.substr(0, entPos + 1));
            sourceString.remove_prefix(entPos + 1);  // Remove '&name;'
            previousPrependRemaining -= Min(previousPrependRemaining, entPos + 1);
          }
        }
      }
    }

    return decodedText;
  }

  ParseResult<ChunkedString> consumePCDataOnce() {
    return consumeAndExpandEntities<TextPredicate, TextNoEntityPredicate>(
        components::EntityType::General, remaining_);
  }

  template <char Quote>
  ParseResult<ChunkedString> consumeAttributeExpandEntities() {
    return consumeAndExpandEntities<QuotedStringPredicate<Quote>,
                                    QuotedStringNoEntityPredicate<Quote>>(
        components::EntityType::General, remaining_);
  }

  bool tryConsume(ChunkedString& sourceString, std::string_view token) {
    if (sourceString.starts_with(token)) {
      sourceString.remove_prefix(token.size());
      return true;
    }

    return false;
  }

  [[nodiscard]] std::optional<char> peek(ChunkedString& sourceString) {
    if (sourceString.empty()) {
      return std::nullopt;
    }

    return sourceString[0];
  };

  // Parse BOM, if any
  void parseBOM() {
    // UTF-8?
    (void)tryConsume(remaining_, "\xEF\xBB\xBF");  // Skip utf-8 bom
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
  std::optional<ChunkedString> consumeContentsUntilEndString(std::string_view endString) {
    auto it = remaining_.find(endString);
    if (it == std::string_view::npos) {
      return std::nullopt;
    }

    ChunkedString result = remaining_.substr(0, it);
    remaining_.remove_prefix(it + endString.size());
    return result;
  }

  /// Parse XML declaration (<?xml...)
  ParseResult<XMLNode> parseXMLDeclaration(FileOffset startOffset) {
    // Create declaration
    XMLNode declaration = XMLNode::CreateXMLDeclarationNode(document_);
    declaration.setSourceStartOffset(startOffset);

    // Skip whitespace before attributes or ?>
    skipWhitespace(remaining_);

    // Parse declaration attributes
    if (auto maybeError = parseNodeAttributes(declaration)) {
      return std::move(maybeError.value());
    }

    // Skip ?>
    if (!tryConsume(remaining_, "?>")) {
      return createParseError("XML declaration missing closing '?>'");
    }

    declaration.setSourceEndOffset(currentOffsetWithLineNumber(remaining_));
    return declaration;
  }

  // Parse XML comment (<!--...)
  ParseResult<std::optional<XMLNode>> parseComment(FileOffset startOffset) {
    const auto maybeComment = consumeContentsUntilEndString("-->");
    if (!maybeComment) {
      return createParseError("Comment node does not end with '-->'");
    }

    const ChunkedString commentStr = maybeComment.value();

    // If Comment nodes are enabled
    if (options_.parseComments) {
      XMLNode commentNode = XMLNode::CreateCommentNode(document_, commentStr.toSingleRcString());
      commentNode.setSourceStartOffset(startOffset);
      commentNode.setSourceEndOffset(currentOffsetWithLineNumber(remaining_));
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
    // We read until the first '>' at nesting level 0, while also handling the internal subset
    // `[...]`
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
      } else if (options_.parseCustomEntities && inInternalSubset && i + 8 < remaining_.size() &&
                 remaining_.substr(i, 8) == std::string_view("<!ENTITY")) {
        // Find the matching '>' that is not inside quotes
        const size_t closePos = FindEntityDeclEnd(remaining_, i + 8);
        if (closePos == std::string_view::npos) {
          return createParseError("Unterminated <!ENTITY declaration in DOCTYPE");
        }

        // Grab the entire substring <!...>
        ChunkedString entityDecl = remaining_.substr(i, closePos - i + 1);

        if (auto maybeErr = parseEntityDeclInDoctype(entityDecl)) {
          return maybeErr.value();
        }

        // Advance i to the '>' so the outer for-loop can continue
        i = closePos;
      }

      i++;
    }

    if (!foundEnd) {
      if (inInternalSubset) {
        return createParseError("Doctype node missing closing ']'");
      } else {
        return createParseError("Doctype node missing closing '>'");
      }
    }

    // The substring includes everything up to `i`
    ChunkedString doctypeStr = remaining_.substr(0, i);
    remaining_.remove_prefix(i + 1);  // consume the '>' as well

    if (options_.parseDoctype) {
      XMLNode docNode = XMLNode::CreateDocTypeNode(document_, doctypeStr.toSingleRcString());
      docNode.setSourceStartOffset(startOffset);
      docNode.setSourceEndOffset(currentOffsetWithLineNumber(remaining_));
      return std::optional<XMLNode>(docNode);
    } else {
      return std::optional<XMLNode>(std::nullopt);
    }
  }

  // Parse PI nodes, e.g. `<?php ... ?>`
  ParseResult<std::optional<XMLNode>> parseProcessingInstructions(FileOffset startOffset) {
    // Extract PI target name
    const ChunkedString piName = consumeMatching<NamePredicate>(remaining_);
    if (piName.empty()) {
      return createParseError("PI target does not begin with a name, e.g. '<?tag'");
    }

    // Skip whitespace after the PI name.
    skipWhitespace(remaining_);

    // Consume contents until finding a '?>'
    const auto maybePiValue = consumeContentsUntilEndString("?>");
    if (!maybePiValue) {
      return createParseError("PI node does not end with '?>'");
    }

    const ChunkedString& piValue = maybePiValue.value();

    if (options_.parseProcessingInstructions) {
      XMLNode pi = XMLNode::CreateProcessingInstructionNode(document_, piName.toSingleRcString(),
                                                            piValue.toSingleRcString());
      pi.setSourceStartOffset(startOffset);
      pi.setSourceEndOffset(currentOffsetWithLineNumber(remaining_));
      return std::make_optional(pi);
    } else {
      return std::optional<XMLNode>(std::nullopt);
    }
  }

  /**
   * Read raw text (PCDATA) until `<` or `\0`
   */
  std::optional<ParseError> parseAndAppendData(XMLNode& node) {
    // Expand all entities in the current text chunk
    auto maybeData = consumePCDataOnce();
    if (maybeData.hasError()) {
      return maybeData.error();
    }

    ChunkedString& dataStr = maybeData.result();

    if (!dataStr.empty()) {
      const RcString dataStrAllocated = dataStr.toSingleRcString();

      // Create new data node
      XMLNode data = XMLNode::CreateDataNode(document_, dataStrAllocated);
      node.appendChild(data);

      // Add data to parent node as well
      node.setValue(dataStrAllocated);
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

    const ChunkedString& cdataStr = maybeCData.value();

    XMLNode cdata = XMLNode::CreateCDataNode(document_, cdataStr.toSingleRcString());
    cdata.setSourceStartOffset(startOffset);
    cdata.setSourceEndOffset(currentOffsetWithLineNumber(remaining_));
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
  std::optional<ParseError> parseEntityDeclInDoctype(ChunkedString& decl) {
    // The string starts with `<!ENTITY` ... ends with '>'
    static constexpr std::string_view kPrefix = "<!ENTITY";
    assert(decl.starts_with(kPrefix) && "Expected '<!ENTITY' in parseEntityDeclInDoctype");
    decl.remove_prefix(kPrefix.size());

    // Skip whitespace
    skipWhitespace(decl);

    components::EntityType entityType = components::EntityType::General;
    if (tryConsume(decl, "%")) {
      entityType = components::EntityType::Parameter;
      skipWhitespace(decl);
    }

    // Parse entity name
    ChunkedString entityName = consumeMatching<NameNoColonPredicate>(decl);
    if (entityName.empty()) {
      return createParseError("Expected entity name");
    }

    skipWhitespace(decl);

    bool isExternal = false;
    ChunkedString entityValue;

    // Check if "SYSTEM" or "PUBLIC"
    if (tryConsume(decl, "SYSTEM") || tryConsume(decl, "PUBLIC")) {
      isExternal = true;
      skipWhitespace(decl);
    }

    const char quote = decl[0];
    if (quote == '"' || quote == '\'') {
      // Parse system identifier
      decl.remove_prefix(1);

      // Read until end quote.
      if (quote == '"') {
        entityValue = consumeMatching<QuotedStringPredicate<'"'>>(decl);
      } else {
        entityValue = consumeMatching<QuotedStringPredicate<'\''>>(decl);
      }

      assert(peek(decl) == quote);
      decl.remove_prefix(1);  // Remove closing quote

    } else {
      return createParseError("Expected quoted string in entity decl");
    }

    // Resolve parameter entity references
    auto maybePieces = consumeAndExpandEntities<AnyPredicate, NoParameterEntityPredicate>(
        components::EntityType::Parameter, entityValue);
    if (maybePieces.hasError()) {
      return maybePieces.error();
    }

    if (!tryConsume(decl, ">")) {
      return createParseError("Expected '>' at end of entity declaration");
    }

    RcString expandedEntityValue = maybePieces.result().toSingleRcString();

    // Store in the entity declarations
    entityCtx_.addEntityDeclaration(entityType, entityName.toSingleRcString(), expandedEntityValue,
                                    isExternal);
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
    skipWhitespace(remaining_);

    // Parse attributes, if any
    if (auto maybeError = parseNodeAttributes(element)) {
      return std::move(maybeError.value());
    }

    // Determine ending type
    if (tryConsume(remaining_, ">")) {
      if (auto maybeError = parseNodeContents(element)) {
        return std::move(maybeError.value());
      }

    } else if (tryConsume(remaining_, "/>")) {
      // Self-closing tag
    } else {
      return createParseError("Node not closed with '>' or '/>'");
    }

    element.setSourceEndOffset(currentOffsetWithLineNumber(remaining_));
    return element;
  }

  /**
   * Parse a node, dispatch on what comes after `<`
   */
  ParseResult<std::optional<XMLNode>> parseNode(FileOffset startOffset) {
    // Parse proper node type
    switch (peek(remaining_).value_or('\0')) {
      default:
        // Parse and append element node
        return parseElement(startOffset).template map<std::optional<XMLNode>>([](auto result) {
          return std::make_optional(result);
        });

      case '?':
        remaining_.remove_prefix(1);  // Skip '?'
        if (tryConsume(remaining_, "xml")) {
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
        if (tryConsume(remaining_, "!--")) {
          // '<!--' - XML comment
          return parseComment(startOffset);
        } else if (tryConsume(remaining_, "![CDATA[")) {
          // '<![CDATA[' - CDATA
          return parseCData(startOffset).template map<std::optional<XMLNode>>([](auto result) {
            return std::make_optional(result);
          });
        } else if (tryConsume(remaining_, "!DOCTYPE")) {
          // '<!DOCTYPE' - DOCTYPE

          if (!isWhitespace(peek(remaining_).value_or('\0'))) {
            return createParseError("Expected whitespace after '<!DOCTYPE'");
          }

          skipWhitespace(remaining_);
          return parseDoctype(startOffset);
        } else {
          return createParseError("Unrecognized node starting with '<!'");
        }

        UTILS_UNREACHABLE();  // All cases above should return.
    }
  }

  ParseResult<XMLQualifiedNameRef> consumeQualifiedName() {
    const ChunkedString name = consumeMatching<NameNoColonPredicate>(remaining_);
    if (name.empty()) {
      return createParseError("Expected qualified name, found invalid character");
    }

    if (tryConsume(remaining_, ":")) {
      // Namespace prefix found
      const ChunkedString localName = consumeMatching<NameNoColonPredicate>(remaining_);
      if (localName.empty()) {
        return createParseError("Expected local part of name after ':', found invalid character");
      }

      return XMLQualifiedNameRef(RcStringOrRef(name.toSingleRcString()),
                                 RcStringOrRef(localName.toSingleRcString()));
    } else {
      return XMLQualifiedNameRef(RcStringOrRef(name.toSingleRcString()));
    }
  }

  /**
   * Parse contents of the node, gather child nodes or text until `</tag>`
   */
  [[nodiscard]] std::optional<ParseError> parseNodeContents(XMLNode& node) {
    // For all children and text
    while (true) {
      // Skip whitespace between > and node contents
      ChunkedString contentsStart = remaining_;
      skipWhitespace(remaining_);
      std::optional<char> nextChar = peek(remaining_);

      if (!nextChar || nextChar == '\0') {
        return createParseError("Unexpected end of data parsing node contents");
      }

      if (nextChar == '<') {
        if (tryConsume(remaining_, "</")) {  // Node closing
          const FileOffset closingTagStart = currentOffsetWithLineNumber(remaining_);

          auto maybeClosingName = consumeQualifiedName();
          if (maybeClosingName.hasError()) {
            ParseError err;
            err.reason = "Invalid closing tag name: " + maybeClosingName.error().reason;
            err.location = maybeClosingName.error().location;
            return err;
          }

          if (node.tagName() != maybeClosingName.result()) {
            return createParseError("Mismatched closing tag", closingTagStart);
          }

          skipWhitespace(remaining_);

          if (!tryConsume(remaining_, ">")) {
            return createParseError("Expected '>' for closing tag");
          }

          return std::nullopt;  // Node closed, finished parsing contents
        } else {
          FileOffset startOffset = currentOffsetWithLineNumber(remaining_);

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
    if (!NameNoColonPredicate::test(peek(remaining_).value_or('\0'))) {
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
    skipWhitespace(remaining_);

    // Skip =
    if (!tryConsume(remaining_, "=")) {
      return createParseError("Attribute name without value, expected '=' followed by a string");
    }

    // Skip whitespace after =
    skipWhitespace(remaining_);

    // Skip quote and remember if it was ' or "
    auto maybeQuote = peek(remaining_);
    if (!maybeQuote || (*maybeQuote != '\'' && *maybeQuote != '"')) {
      return createParseError("Attribute value not enclosed in quotes, expected \" or '");
    }

    const char quote = maybeQuote.value();
    remaining_.remove_prefix(1);

    // Extract attribute value and expand char refs in it
    auto maybeValue = quote == '\'' ? consumeAttributeExpandEntities<'\''>()
                                    : consumeAttributeExpandEntities<'"'>();
    if (maybeValue.hasError()) {
      return std::move(maybeValue.error());
    }

    // Make sure that end quote is present
    if (!tryConsume(remaining_, std::string_view(&quote, 1))) {
      if (quote == '\'') {
        return createParseError("Attribute value not closed with \"'\"");
      } else {
        return createParseError("Attribute value not closed with '\"'");
      }
    }

    ParsedAttribute result{name, maybeValue.result().toSingleRcString()};
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

      skipWhitespace(remaining_);

      if (maybeAttribute.result().has_value()) {
        const ParsedAttribute& attribute = maybeAttribute.result().value();
        node.setAttribute(attribute.name, attribute.value);
      } else {
        break;
      }
    }

    // Skip whitespace after attributes.
    skipWhitespace(remaining_);

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
