#include "src/css/parser/value_parser.h"

#include "src/css/parser/details/subparsers.h"
#include "src/css/parser/details/tokenizer.h"

namespace donner {
namespace css {

std::vector<ComponentValue> ValueParser::Parse(std::string_view str) {
  details::Tokenizer tokenizer_(str);
  return details::parseListOfComponentValues(tokenizer_);
}

}  // namespace css
}  // namespace donner