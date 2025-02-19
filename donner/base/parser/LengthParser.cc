#include "donner/base/parser/LengthParser.h"

#include <algorithm>

#include "donner/base/parser/details/ParserBase.h"

namespace donner::parser {

namespace {

std::optional<Lengthd::Unit> parseUnit(std::string_view suffix, size_t* charsConsumed) {
  struct SuffixMap {
    Lengthd::Unit unit;
    std::string_view suffix;
  };

  // Note: suffix must be lowercase for comparison.

  // This can potentially be optimized to use a binary search, however for the size of this list it
  // would likely not be worth it. It isn't a good candidate for a frozen map because we're trying
  // to match string prefixes.
  static constexpr const SuffixMap kSuffixMap[] = {
      {Lengthd::Unit::Percent, "%"},  //
      {Lengthd::Unit::Cm, "cm"},      //
      {Lengthd::Unit::Mm, "mm"},      //
      {Lengthd::Unit::Q, "q"},        //
      {Lengthd::Unit::In, "in"},      //
      {Lengthd::Unit::Pc, "pc"},      //
      {Lengthd::Unit::Pt, "pt"},      //
      {Lengthd::Unit::Px, "px"},      //
      {Lengthd::Unit::Em, "em"},      //
      {Lengthd::Unit::Ex, "ex"},      //
      {Lengthd::Unit::Ch, "ch"},      //
      {Lengthd::Unit::Rem, "rem"},    //
      {Lengthd::Unit::Vw, "vw"},      //
      {Lengthd::Unit::Vh, "vh"},      //
      {Lengthd::Unit::Vmin, "vmin"},  //
      {Lengthd::Unit::Vmax, "vmax"},  //
  };
  const size_t kMaxLength = 4;

  // Comparisons are case-insensitive, convert token to lowercase.
  std::string token = std::string(suffix.substr(0, std::min(suffix.size(), kMaxLength)));
  std::transform(token.begin(), token.end(), token.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  for (const auto& unit : kSuffixMap) {
    if (token.starts_with(unit.suffix)) {
      *charsConsumed = unit.suffix.size();
      return unit.unit;
    }
  }

  return std::nullopt;
}

}  // namespace

/**
 * Implementation of the LengthParser.
 */
class LengthParserImpl : public ParserBase {
public:
  /**
   * Construct a new LengthParser implementation object.
   *
   * @param str String to parse.
   * @param options Parser options.
   */
  LengthParserImpl(std::string_view str, LengthParser::Options options)
      : ParserBase(str), options_(options) {}

  /**
   * Parse a length and return the result or error.
   */
  ParseResult<LengthParser::Result> parse() {
    LengthParser::Result result;

    auto maybeNumber = readNumber();
    if (maybeNumber.hasError()) {
      return std::move(maybeNumber.error());
    }

    const double number = maybeNumber.result();
    if (remaining_.empty() || isWhitespace(remaining_[0])) {
      if (unitRequired(number)) {
        ParseError err;
        err.reason = "Unit expected";
        err.location = currentOffset();
        return err;
      }

      result.length.value = number;
      result.consumedChars = consumedChars();
      return result;
    }

    size_t charsConsumed = 0;
    if (auto maybeUnit = parseUnit(remaining_, &charsConsumed)) {
      remaining_.remove_prefix(charsConsumed);
      result.consumedChars = consumedChars();
      result.length.value = number;
      result.length.unit = maybeUnit.value();

      if (options_.limitUnitToPercentage && result.length.unit != Lengthd::Unit::Percent) {
        ParseError err;
        err.reason = "Unexpected unit, expected percentage";
        err.location = currentOffset();
        return err;
      }

      return result;
    }

    if (unitRequired(number)) {
      ParseError err;
      err.reason = "Invalid unit";
      err.location = currentOffset();
      return err;
    } else {
      result.length.value = number;
      result.consumedChars = consumedChars();
      return result;
    }
  }

  /**
   * Check if a unit is required for the given number.
   *
   * @param number Number to check.
   */
  bool unitRequired(double number) const { return !(number == 0.0 || options_.unitOptional); }

private:
  LengthParser::Options options_;
};

ParseResult<LengthParser::Result> LengthParser::Parse(std::string_view str,
                                                      LengthParser::Options options) {
  LengthParserImpl parser(str, options);
  return parser.parse();
}

std::optional<Lengthd::Unit> LengthParser::ParseUnit(std::string_view str) {
  size_t charsConsumed = 0;
  if (auto maybeUnit = parseUnit(str, &charsConsumed)) {
    if (charsConsumed == str.size()) {
      return maybeUnit;
    }
  }

  return std::nullopt;
}

}  // namespace donner::parser
