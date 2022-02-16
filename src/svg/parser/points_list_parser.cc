#include "src/svg/parser/points_list_parser.h"

#include <vector>

#include "src/base/parser/details/parser_base.h"
#include "src/base/parser/number_parser.h"

namespace donner::svg {

class PointsListParserImpl : public ParserBase {
public:
  PointsListParserImpl(std::string_view str) : ParserBase(str) {}

  ParseResult<std::vector<Vector2d>> parse() {
    skipWhitespace();

    while (!remaining_.empty()) {
      if (!points_.empty()) {
        // Allow commas after the first coordinate.
        skipCommaWhitespace();

        // To provide better error messages, detect an extraneous comma here.
        if (remaining_.starts_with(',')) {
          ParseError err;
          err.reason = "Extra ',' before coordinate";
          err.offset = currentOffset();
          return err;
        }
      }

      auto maybeX = readNumber();
      if (maybeX.hasError()) {
        return resultAndError(std::move(maybeX.error()));
      }

      skipCommaWhitespace();

      auto maybeY = readNumber();
      if (maybeY.hasError()) {
        return resultAndError(std::move(maybeY.error()));
      }

      points_.emplace_back(maybeX.result(), maybeY.result());
    }

    return std::move(points_);
  }

private:
  ParseResult<std::vector<Vector2d>> resultAndError(ParseError&& error) {
    return ParseResult<std::vector<Vector2d>>(std::move(points_), std::move(error));
  }

  std::vector<Vector2d> points_;
};

ParseResult<std::vector<Vector2d>> PointsListParser::Parse(std::string_view str) {
  PointsListParserImpl parser(str);
  return parser.parse();
}

}  // namespace donner::svg
