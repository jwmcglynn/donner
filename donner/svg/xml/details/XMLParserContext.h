#pragma once

#include <vector>

#include "donner/base/parser/ParseError.h"
#include "donner/svg/xml/XMLParser.h"
#include "donner/svg/xml/details/LineOffsets.h"

namespace donner::svg::parser {

/**
 * Contains the start location within a string where a subparser was invoked, used for remapping
 * errors back to their original text.
 */
struct ParserOrigin {
  /// 0-based offset into the string where the subparser started.
  size_t startOffset;

  /**
   * Create a ParserOrigin with the given start offset.
   *
   * @param offset 0-based offset into the string where the subparser started.
   */
  static ParserOrigin StartOffset(size_t offset) { return ParserOrigin{offset}; }
};

/**
 * Stores the current state of \ref XMLParser during parsing. Used to add parse warnings and
 * store global state like the parsing options.
 */
class XMLParserContext {
public:
  /**
   * Construct a new context for the given input string.
   *
   * @param input Input string.
   * @param warningsStorage Storage for warnings, may be \c nullptr to disable warnings.
   * @param options Options for parsing.
   */
  XMLParserContext(std::string_view input, std::vector<ParseError>* warningsStorage,
                   const XMLParser::Options& options)
      : input_(input), lineOffsets_(input), warnings_(warningsStorage), options_(options) {}

  /// Get the parser options.
  const XMLParser::Options& options() const { return options_; }

  /**
   * Set the XML document's default namespace prefix, such as "http://www.w3.org/2000/svg".
   *
   * @param namespacePrefix The default namespace prefix, such as "http://www.w3.org/2000/svg".
   */
  void setNamespacePrefix(std::string_view namespacePrefix) { namespacePrefix_ = namespacePrefix; }

  /// Get the XML document's default namespace prefix, such as "http://www.w3.org/2000/svg".
  std::string_view namespacePrefix() const { return namespacePrefix_; }

  /**
   * Remap a parse error from a subparser back to the original input string, translating the line
   * numbers.
   *
   * @param error Error to remap.
   * @param origin Origin of the subparser.
   * @return ParseError Remapped error.
   */
  ParseError fromSubparser(ParseError&& error, ParserOrigin origin) {
    const size_t line = lineOffsets_.offsetToLine(origin.startOffset);

    ParseError newError = std::move(error);
    if (newError.location.line == 0) {
      assert(newError.location.offset.has_value() &&
             "Location must be resolved and not have a nullopt offset");
      newError.location.offset.value() += origin.startOffset - lineOffsets_.lineOffset(line);
    }
    newError.location.line += line;
    return newError;
  }

  /**
   * Add a warning to the list of warnings.
   *
   * @param warning Warning to add.
   */
  void addWarning(ParseError&& warning) {
    if (warnings_) {
      warnings_->emplace_back(std::move(warning));
    }
  }

  /**
   * Add a warning from a subparser to the list of warnings, remapping the error back to the
   * original input string.
   *
   * @param warning Warning to add.
   * @param origin Origin of the subparser.
   */
  void addSubparserWarning(ParseError&& warning, ParserOrigin origin) {
    addWarning(fromSubparser(std::move(warning), origin));
  }

  /**
   * Create a \ref ParserOrigin for the given substring, where \p substring is within the XML
   * parser's original string, \ref input_.
   *
   * @param substring Substring within the XML parser's original string.
   */
  ParserOrigin parserOriginFrom(std::string_view substring) const {
    if (substring.begin() > input_.begin() && substring.end() < input_.end()) {
      return ParserOrigin::StartOffset(substring.begin() - input_.begin());
    } else {
      return ParserOrigin::StartOffset(0);
    }
  }

  /**
   * Return line numbers for the given offset.
   *
   * For example, given a string: "abc\n123", offsets 0-3 would be considered line 1, and offsets
   * after 4 (corresponding to the index of '1'), would be line 2. Values beyond the length of the
   * string return the last line number.
   *
   * @param offset Character index.
   * @return size_t Line number, 1-indexed.
   */
  size_t offsetToLine(size_t offset) const { return lineOffsets_.offsetToLine(offset); }

  /**
   * Returns the offset of a given 1-indexed line number.
   *
   * @param line Line number, 1-indexed.
   */
  size_t lineOffset(size_t line) const { return lineOffsets_.lineOffset(line); }

private:
  /// Original string containing the XML text, used for remapping errors.
  std::string_view input_;

  /// Offsets of the start of each line in the input string.
  LineOffsets lineOffsets_;

  /// Storage for warnings, may be \c nullptr to disable warnings.
  std::vector<ParseError>* warnings_;

  /// Options for parsing.
  XMLParser::Options options_;

  /// The XML document's default namespace prefix, such as "http://www.w3.org/2000/svg".
  std::string_view namespacePrefix_;
};

}  // namespace donner::svg::parser
