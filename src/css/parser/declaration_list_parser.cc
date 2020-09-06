#include "src/css/parser/declaration_list_parser.h"

#include "src/base/parser/details/parser_base.h"

namespace donner {
namespace css {

class DeclarationListParserImpl : public ParserBase {
public:
  DeclarationListParserImpl(std::string_view str) : ParserBase(str) {}

  ParseResult<std::vector<Declaration>> parse() {
    ParseError err;
    err.reason = "Not implemented";
    err.offset = currentOffset();
    return err;
  }
};

ParseResult<std::vector<Declaration>> DeclarationListParser::Parse(std::string_view str) {
  DeclarationListParserImpl parser(str);
  return parser.parse();
}

}  // namespace css
}  // namespace donner
