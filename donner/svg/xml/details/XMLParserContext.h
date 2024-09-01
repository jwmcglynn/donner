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
   * @param startOffset 0-based offset into the string where the subparser started.
   */
  static ParserOrigin StartOffset(size_t offset) { return ParserOrigin{offset}; }
};

class XMLParserContext {
public:
  XMLParserContext(std::string_view input, std::vector<ParseError>* warningsStorage,
                   const XMLParser::Options& options)
      : input_(input), lineOffsets_(input), warnings_(warningsStorage), options_(options) {}

  XMLParser::Options options() const { return options_; }

  void setNamespacePrefix(std::string_view namespacePrefix) { namespacePrefix_ = namespacePrefix; }

  std::string_view namespacePrefix() const { return namespacePrefix_; }

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

  void addWarning(ParseError&& warning) {
    if (warnings_) {
      warnings_->emplace_back(std::move(warning));
    }
  }

  void addSubparserWarning(ParseError&& warning, ParserOrigin origin) {
    addWarning(fromSubparser(std::move(warning), origin));
  }

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
   */
  size_t lineOffset(size_t line) const { return lineOffsets_.lineOffset(line); }

private:
  std::string_view input_;
  LineOffsets lineOffsets_;
  std::vector<ParseError>* warnings_;
  XMLParser::Options options_;

  std::string_view namespacePrefix_;
};

}  // namespace donner::svg::parser
