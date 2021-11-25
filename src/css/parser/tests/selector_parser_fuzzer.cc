#include "src/css/parser/selector_parser.h"

namespace donner {
namespace css {

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = SelectorParser::Parse(std::string_view(reinterpret_cast<const char*>(data), size));
  (void)result;

  return 0;
}

}  // namespace css
}  // namespace donner
