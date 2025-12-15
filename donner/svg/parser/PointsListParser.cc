#include "donner/svg/parser/PointsListParser.h"

#include <vector>

#include "donner/base/parser/details/ParserBase.h"

namespace donner::svg::parser {

namespace {

/// Implementation of \ref PointsListParser.
class PointsListParserImpl : public donner::parser::ParserBase {
public:
  /**
   * Construct a PointsListParserImpl.
   *
   * @param str The string to parse.
   * @param outWarning Optional destination for a warning emitted when parsing stops early. When
   *                   provided, parsing succeeds with the partial list and records the reason in
   *                   `outWarning`.
   */
  explicit PointsListParserImpl(std::string_view str, std::optional<ParseError>* outWarning)
      : ParserBase(str), outWarning_(outWarning) {}

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
        return returnEarlyWithWarning(std::move(maybeX.error()));
      }

      skipCommaWhitespace();

      auto maybeY = readNumber();
      if (maybeY.hasError()) {
        return returnEarlyWithWarning(std::move(maybeY.error()));
      }

      points_.emplace_back(maybeX.result(), maybeY.result());
    }

    return std::move(points_);
  }

private:
  /**
   * Handle a non-critical parse error by returning the partial path data and capturing the warning.
   * If the points list is empty this is considered a critical error and the warning is upgraded.
   */
  ParseResult<std::vector<Vector2d>> returnEarlyWithWarning(ParseError&& warning) {
    if (points_.empty()) {
      // Critical error: No data was parsed
      return std::move(warning);
    }

    // Non-critical error: We have partial data
    if (outWarning_) {
      outWarning_->emplace(std::move(warning));
    }

    // IMPORTANT: We only want one return statement here, and it should be the result to avoid
    // having different behavior if outWarnings_ is null or not.
    return std::move(points_);
  }

  std::vector<Vector2d> points_;

  std::optional<ParseError>* outWarning_;
};

}  // namespace

ParseResult<std::vector<Vector2d>> PointsListParser::Parse(std::string_view str,
                                                           std::optional<ParseError>* outWarning) {
  PointsListParserImpl parser(str, outWarning);
  return parser.parse();
}

}  // namespace donner::svg::parser
