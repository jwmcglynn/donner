#include "src/css/parser/stylesheet_parser.h"

#include "src/css/parser/details/subparsers.h"
#include "src/css/parser/details/tokenizer.h"

namespace donner {
namespace css {

namespace {

class StylesheetParserImpl {
public:
  StylesheetParserImpl(std::string_view str) : tokenizer_(str) {}

  Stylesheet parse() {
    Stylesheet result;

    while (!tokenizer_.isEOF()) {
      auto token = tokenizer_.next();
    }

    return result;
  }

private:
  details::Tokenizer tokenizer_;
};

}  // namespace

Stylesheet StylesheetParser::Parse(std::string_view str) {
  StylesheetParserImpl parser(str);
  return parser.parse();
}

}  // namespace css
}  // namespace donner
