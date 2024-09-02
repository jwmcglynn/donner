#include "donner/svg/parser/PointsListParser.h"

#include <vector>

#include "donner/base/parser/details/ParserBase.h"

namespace donner::svg::parser {

namespace {

/// Implementation of \ref PointsListParser.
class PointsListParserImpl : public base::parser::ParserBase {
public:
  /**
   * Construct a PointsListParserImpl.
   *
   * @param str The string to parse.
   */
  explicit PointsListParserImpl(std::string_view str) : ParserBase(str) {}

  /**
   * Parse the points list.
   *
   * @return The parsed points list, or an error if parsing failed.
   */
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
          err.location = currentOffset();
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

}  // namespace

ParseResult<std::vector<Vector2d>> PointsListParser::Parse(std::string_view str) {
  PointsListParserImpl parser(str);
  return parser.parse();
}

}  // namespace donner::svg::parser
