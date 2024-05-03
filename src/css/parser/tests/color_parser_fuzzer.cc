#include "src/css/parser/color_parser.h"

namespace donner::css {

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result =
      ColorParser::ParseString(std::string_view(reinterpret_cast<const char*>(data), size));
  (void)result;

  return 0;
}

}  // namespace donner::css
