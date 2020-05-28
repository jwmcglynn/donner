#include "src/svg/parser/path_parser.h"

namespace donner {

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = PathParser::parse(std::string_view(reinterpret_cast<const char*>(data), size));
  (void)result;

  return 0;
}

}  // namespace donner
