#include "src/css/parser/value_parser.h"

#include "src/css/parser/details/subparsers.h"
#include "src/css/parser/details/tokenizer.h"

namespace donner::css {

std::vector<ComponentValue> ValueParser::Parse(std::string_view str) {
  details::Tokenizer tokenizer(str);
  return details::parseListOfComponentValues(tokenizer,
                                             details::WhitespaceHandling::TrimLeadingAndTrailing);
}

}  // namespace donner::css
