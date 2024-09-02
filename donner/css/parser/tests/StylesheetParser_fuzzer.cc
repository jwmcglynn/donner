#include "donner/css/parser/StylesheetParser.h"

namespace donner::css::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = StylesheetParser::Parse(
      std::string_view(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                       size));
  (void)result;

  return 0;
}

}  // namespace donner::css::parser
