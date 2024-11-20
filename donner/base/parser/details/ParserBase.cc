#include "donner/base/parser/details/ParserBase.h"

#include "donner/base/parser/NumberParser.h"

namespace donner::parser {

ParserBase::ParserBase(std::string_view str) : str_(str), remaining_(str) {}

std::string_view ParserBase::take(size_t count) {
  std::string_view result = remaining_.substr(0, count);
  remaining_.remove_prefix(count);
  return result;
}

void ParserBase::skipWhitespace() {
  while (!remaining_.empty() && isWhitespace(remaining_[0])) {
    remaining_.remove_prefix(1);
  }
}

void ParserBase::skipCommaWhitespace() {
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

bool ParserBase::isWhitespace(char ch) const {
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

FileOffset ParserBase::currentOffset(int index) const {
  return FileOffset::Offset(consumedChars() + index);
}

size_t ParserBase::consumedChars() const {
  return remaining_.data() - str_.data();
}

ParseResult<double> ParserBase::readNumber() {
  auto maybeResult = NumberParser::Parse(remaining_);
  if (maybeResult.hasError()) {
    ParseError err = std::move(maybeResult.error());
    err.location = err.location.addParentOffset(currentOffset());
    return err;
  }

  const NumberParser::Result& result = maybeResult.result();
  remaining_.remove_prefix(result.consumedChars);
  return result.number;
}

std::optional<ParseError> ParserBase::readNumbers(std::span<double> resultStorage) {
  for (size_t i = 0; i < resultStorage.size(); ++i) {
    if (i != 0) {
      ParserBase::skipCommaWhitespace();
    }

    auto maybeNumber = ParserBase::readNumber();
    if (maybeNumber.hasError()) {
      return std::move(maybeNumber.error());
    }

    resultStorage[i] = maybeNumber.result();
  }

  return std::nullopt;
}

}  // namespace donner::parser
