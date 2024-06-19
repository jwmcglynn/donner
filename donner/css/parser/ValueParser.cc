#include "donner/css/parser/ValueParser.h"

#include "donner/css/parser/details/Subparsers.h"
#include "donner/css/parser/details/Tokenizer.h"

namespace donner::css::parser {

std::vector<ComponentValue> ValueParser::Parse(std::string_view str) {
  details::Tokenizer tokenizer(str);
  return details::parseListOfComponentValues(tokenizer,
                                             details::WhitespaceHandling::TrimLeadingAndTrailing);
}

}  // namespace donner::css::parser
