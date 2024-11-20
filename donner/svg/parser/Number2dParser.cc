#include "donner/svg/parser/Number2dParser.h"

#include "donner/base/parser/NumberParser.h"

namespace donner::svg::parser {

namespace {

static bool isWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\f' || ch == '\r' || ch == '\n';
}

}  // namespace

ParseResult<Number2dParser::Result> Number2dParser::Parse(std::string_view str) {
  using donner::parser::NumberParser;

  NumberParser::Options options;
  options.forbidOutOfRange = false;

  const auto maybeResultX = NumberParser::Parse(str, options);
  if (maybeResultX.hasError()) {
    return ParseError(maybeResultX.error());
  }

  const double numberX = maybeResultX.result().number;
  std::string_view remainingStr = str.substr(maybeResultX.result().consumedChars);

  // Trim whitespace
  while (!remainingStr.empty() && isWhitespace(remainingStr[0])) {
    remainingStr.remove_prefix(1);
  }

  if (remainingStr.empty()) {
    return Number2dParser::Result{numberX, numberX, maybeResultX.result().consumedChars};
  }

  const auto maybeResultY = NumberParser::Parse(remainingStr, options);
  if (maybeResultY.hasError()) {
    return ParseError(maybeResultY.error());
  }

  const double numberY = maybeResultY.result().number;
  remainingStr.remove_prefix(maybeResultY.result().consumedChars);

  return Number2dParser::Result{numberX, numberY, str.size() - remainingStr.size()};
}

}  // namespace donner::svg::parser
