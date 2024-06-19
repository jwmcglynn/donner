#include "donner/svg/parser/PathParser.h"

namespace donner::svg::parser {

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = PathParser::Parse(
      std::string_view(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                       size));
  (void)result;

  return 0;
}

}  // namespace donner::svg::parser
