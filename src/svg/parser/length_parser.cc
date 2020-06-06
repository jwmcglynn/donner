#include "src/svg/parser/length_parser.h"

#include "src/svg/parser/details/parser_base.h"

namespace donner {

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
      result.consumed_chars = currentOffset();
      return result;
    }

    struct SuffixMap {
      Lengthd::Unit unit;
      std::string_view suffix;
    };

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
    std::string token = std::string(remaining_.substr(0, std::min(remaining_.size(), kMaxLength)));
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& unit : kSuffixMap) {
      if (token.starts_with(unit.suffix)) {
        remaining_.remove_prefix(unit.suffix.size());
        result.consumed_chars = currentOffset();
        result.length.value = number;
        result.length.unit = unit.unit;
        return result;
      }
    }

    if (unitRequired(number)) {
      ParseError err;
      err.reason = "Invalid unit";
      err.offset = currentOffset();
      return err;
    } else {
      result.length.value = number;
      result.consumed_chars = currentOffset();
      return result;
    }
  }

  bool unitRequired(double number) const { return !(number == 0.0 || options_.unit_optional); }

private:
  LengthParser::Options options_;
};

ParseResult<LengthParser::Result> LengthParser::Parse(std::string_view str,
                                                      LengthParser::Options options) {
  LengthParserImpl parser(str, options);
  return parser.parse();
}

}  // namespace donner
