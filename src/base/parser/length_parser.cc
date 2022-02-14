#include "src/base/parser/length_parser.h"

#include "src/base/parser/details/parser_base.h"

namespace donner {

namespace {

std::optional<Lengthd::Unit> parseUnit(std::string_view suffix, size_t* charsConsumed) {
  struct SuffixMap {
    Lengthd::Unit unit;
    std::string_view suffix;
  };

  // TODO: Switch this to frozen map.
  // Note: suffix must be lowercase for comparison.
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

class LengthParserImpl : public ParserBase {
public:
  LengthParserImpl(std::string_view str, LengthParser::Options options)
      : ParserBase(str), options_(options) {}

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
        err.offset = currentOffset();
        return err;
      }

      result.length.value = number;
      result.consumedChars = currentOffset();
      return result;
    }

    size_t charsConsumed = 0;
    if (auto maybeUnit = parseUnit(remaining_, &charsConsumed)) {
      remaining_.remove_prefix(charsConsumed);
      result.consumedChars = currentOffset();
      result.length.value = number;
      result.length.unit = maybeUnit.value();
      return result;
    }

    if (unitRequired(number)) {
      ParseError err;
      err.reason = "Invalid unit";
      err.offset = currentOffset();
      return err;
    } else {
      result.length.value = number;
      result.consumedChars = currentOffset();
      return result;
    }
  }

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
  if (auto maybeUnit = parseUnit(str, &charsConsumed); maybeUnit && charsConsumed == str.size()) {
    return maybeUnit;
  } else {
    return std::nullopt;
  }
}

}  // namespace donner
