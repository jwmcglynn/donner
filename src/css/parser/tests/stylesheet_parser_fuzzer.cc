#include "src/css/parser/stylesheet_parser.h"

namespace donner::css {

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result =
      StylesheetParser::Parse(std::string_view(reinterpret_cast<const char*>(data), size));
  (void)result;

  return 0;
}

}  // namespace donner::css