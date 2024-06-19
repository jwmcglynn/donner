#include "donner/css/parser/SelectorParser.h"

namespace donner::css::parser {

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = SelectorParser::Parse(
      std::string_view(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                       size));
  (void)result;

  return 0;
}

}  // namespace donner::css::parser
