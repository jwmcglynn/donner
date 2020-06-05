#include "src/svg/parser/viewbox_parser.h"

#include "src/svg/parser/details/parser_base.h"

namespace donner {

class ViewboxParserImpl : public ParserBase {
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
      err.offset = currentOffset();
      return err;
    }

    return Boxd({numbers[0], numbers[1]}, {numbers[0] + numbers[2], numbers[1] + numbers[3]});
  }
};

ParseResult<Boxd> ViewboxParser::Parse(std::string_view str) {
  ViewboxParserImpl parser(str);
  return parser.parse();
}

}  // namespace donner
