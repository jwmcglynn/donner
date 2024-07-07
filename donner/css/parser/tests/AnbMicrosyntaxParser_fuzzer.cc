#include <cstdint>

#include "donner/css/parser/AnbMicrosyntaxParser.h"
#include "donner/css/parser/details/ComponentValueParser.h"

namespace donner::css::parser {

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  details::Tokenizer tokenizer(
      std::string_view(reinterpret_cast<const char*>(data), size));  // NOLINT: Intentional cast
  std::vector<ComponentValue> components =
      details::parseListOfComponentValues(tokenizer, details::WhitespaceHandling::Keep);

  auto result = AnbMicrosyntaxParser::Parse(components);
  (void)result;

  return 0;
}

}  // namespace donner::css::parser
