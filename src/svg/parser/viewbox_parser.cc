#include "src/svg/parser/viewbox_parser.h"

#include <span>

#include "src/svg/parser/number_parser.h"

namespace donner {

class ViewboxParserImpl {
public:
  ViewboxParserImpl(std::string_view str) : str_(str), remaining_(str) {}

  ParseResult<Boxd> parse() {
    double numbers[4];
    if (auto error = readNumbers(numbers)) {
      return std::move(error.value());
    }

    if (numbers[2] < 0.0 || numbers[3] < 0.0) {
      ParseError err;
      err.reason = "Width and height should be positive";
      return err;
    }

    if (!remaining_.empty()) {
      ParseError err;
      err.reason = "Expected end of string";
      err.offset = currentOffset();
      return err;
    }

    return Boxd({numbers[0], numbers[1]}, {numbers[0] + numbers[2], numbers[1] + numbers[3]});
  }

private:
  void skipCommaWhitespace() {
    bool foundComma = false;
    while (!remaining_.empty()) {
      const char ch = remaining_[0];
      if (!foundComma && ch == ',') {
        foundComma = true;
        remaining_.remove_prefix(1);
      } else if (isWhitespace(ch)) {
        remaining_.remove_prefix(1);
      } else {
        break;
      }
    }
  }

  bool isWhitespace(char ch) const {
    // https://www.w3.org/TR/css-transforms-1/#svg-wsp
    // Either a U+000A LINE FEED, U+000D CARRIAGE RETURN, U+0009 CHARACTER TABULATION, or U+0020
    // SPACE.
    return ch == '\t' || ch == ' ' || ch == '\n' || ch == '\r';
  }

  int currentOffset() { return remaining_.data() - str_.data(); }

  ParseResult<double> readNumber() {
    ParseResult<NumberParser::Result> maybeResult = NumberParser::parse(remaining_);
    if (maybeResult.hasError()) {
      ParseError err = std::move(maybeResult.error());
      err.offset += currentOffset();
      return err;
    }

    const NumberParser::Result& result = maybeResult.result();
    remaining_.remove_prefix(result.consumed_chars);
    return result.number;
  }

  std::optional<ParseError> readNumbers(std::span<double> resultStorage) {
    for (size_t i = 0; i < resultStorage.size(); ++i) {
      if (i != 0) {
        skipCommaWhitespace();
      }

      auto maybeNumber = readNumber();
      if (maybeNumber.hasError()) {
        return std::move(maybeNumber.error());
      }

      resultStorage[i] = maybeNumber.result();
    }

    return std::nullopt;
  }

  const std::string_view str_;
  std::string_view remaining_;
};

ParseResult<Boxd> ViewboxParser::parse(std::string_view str) {
  ViewboxParserImpl parser(str);
  return parser.parse();
}

}  // namespace donner
