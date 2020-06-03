#pragma once

#include <vector>

#include "src/svg/parser/parse_error.h"
#include "src/svg/xml/details/line_offsets.h"

namespace donner {

struct ParserOrigin {
  size_t startOffset;

  static ParserOrigin StartOffset(size_t offset) { return ParserOrigin{offset}; }
};

class XMLParserContext {
public:
  XMLParserContext(std::string_view input, std::vector<ParseError>* warningsStorage)
      : input_(input), line_offsets_(input), warnings_(warningsStorage) {}

  ParseError fromSubparser(ParseError&& error, ParserOrigin origin) {
    const size_t line = line_offsets_.offsetToLine(origin.startOffset);

    ParseError newError = std::move(error);
    if (newError.line == 0) {
      newError.offset += origin.startOffset - line_offsets_.lineOffset(line);
    }
    newError.line += line;
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
  size_t offsetToLine(size_t offset) const { return line_offsets_.offsetToLine(offset); }

  /**
   * Returns the offset of a given 1-indexed line number.
   */
  size_t lineOffset(size_t line) const { return line_offsets_.lineOffset(line); }

private:
  std::string_view input_;
  LineOffsets line_offsets_;
  std::vector<ParseError>* warnings_;
};

}  // namespace donner