#include "donner/css/parser/ColorParser.h"

namespace donner::css::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = ColorParser::ParseString(
      std::string_view(reinterpret_cast<const char*>(data),  // NOLINT: intentional cast
                       size));
  (void)result;

  return 0;
}

}  // namespace donner::css::parser
