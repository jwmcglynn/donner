#include "donner/base/fonts/WoffParser.h"

namespace donner::fonts {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = WoffParser::Parse(std::span<const uint8_t>(data, size));
  (void)result;

  return 0;
}

}  // namespace donner::fonts
