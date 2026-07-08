#include "donner/base/encoding/UrlDecode.h"

namespace donner {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view strView(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                                 size);

  auto result = UrlDecode(strView);
  (void)result;

  return 0;
}

}  // namespace donner
