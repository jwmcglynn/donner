#include "donner/base/fonts/Woff2Parser.h"

namespace donner::fonts {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = Woff2Parser::Decompress(std::span<const uint8_t>(data, size));
  (void)result;

  return 0;
}

}  // namespace donner::fonts
