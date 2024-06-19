#include "donner/svg/parser/ViewboxParser.h"

#include "donner/base/parser/details/ParserBase.h"

namespace donner::svg::parser {

class ViewboxParserImpl : public base::parser::ParserBase {
public:
  ViewboxParserImpl(std::string_view str) : ParserBase(str) {}

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
      err.location = currentOffset();
      return err;
    }

    return Boxd({numbers[0], numbers[1]}, {numbers[0] + numbers[2], numbers[1] + numbers[3]});
  }
};

ParseResult<Boxd> ViewboxParser::Parse(std::string_view str) {
  ViewboxParserImpl parser(str);
  return parser.parse();
}

}  // namespace donner::svg::parser
