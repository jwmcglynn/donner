#include "donner/svg/parser/PreserveAspectRatioParser.h"

#include "donner/base/parser/details/ParserBase.h"

namespace donner::svg::parser {

class PreserveAspectRatioParserImpl : public donner::parser::ParserBase {
public:
  PreserveAspectRatioParserImpl(std::string_view str) : ParserBase(str) {}

  ParseResult<PreserveAspectRatio> parse() {
    PreserveAspectRatio result;

    {
      const std::string_view align = readToken();
      if (align == "none") {
        result.align = PreserveAspectRatio::Align::None;
      } else if (align == "xMinYMin") {
        result.align = PreserveAspectRatio::Align::XMinYMin;
      } else if (align == "xMidYMin") {
        result.align = PreserveAspectRatio::Align::XMidYMin;
      } else if (align == "xMaxYMin") {
        result.align = PreserveAspectRatio::Align::XMaxYMin;
      } else if (align == "xMinYMid") {
        result.align = PreserveAspectRatio::Align::XMinYMid;
      } else if (align == "xMidYMid") {
        result.align = PreserveAspectRatio::Align::XMidYMid;
      } else if (align == "xMaxYMid") {
        result.align = PreserveAspectRatio::Align::XMaxYMid;
      } else if (align == "xMinYMax") {
        result.align = PreserveAspectRatio::Align::XMinYMax;
      } else if (align == "xMidYMax") {
        result.align = PreserveAspectRatio::Align::XMidYMax;
      } else if (align == "xMaxYMax") {
        result.align = PreserveAspectRatio::Align::XMaxYMax;
      } else {
        ParseError err;
        err.reason = align.empty() ? std::string("Unexpected end of string instead of align")
                                   : ("Invalid align: '" + std::string(align) + "'");
        err.location = currentOffset();
        return err;
      }
    }

    skipWhitespace();

    if (!remaining_.empty()) {
      const std::string_view meetOrSlice = readToken();
      if (meetOrSlice == "meet") {
        result.meetOrSlice = PreserveAspectRatio::MeetOrSlice::Meet;
      } else if (meetOrSlice == "slice") {
        result.meetOrSlice = PreserveAspectRatio::MeetOrSlice::Slice;
      } else {
        ParseError err;
        err.reason = "Invalid meetOrSlice: '" + std::string(meetOrSlice) + "'";
        err.location = currentOffset();
        return err;
      }

      if (!remaining_.empty()) {
        ParseError err;
        err.reason = "End of attribute expected";
        err.location = currentOffset();
        return err;
      }
    }

    return result;
  }

protected:
  std::string_view readToken() {
    for (size_t i = 0; i < remaining_.size(); ++i) {
      if (isWhitespace(remaining_[i])) {
        return take(i);
      }
    }

    return take(remaining_.size());
  }
};

ParseResult<PreserveAspectRatio> PreserveAspectRatioParser::Parse(std::string_view str) {
  PreserveAspectRatioParserImpl parser(str);
  return parser.parse();
}

}  // namespace donner::svg::parser
