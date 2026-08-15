#include "donner/svg/text/FontDataUtils.h"

namespace donner::svg {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> fontData(data, size);
  (void)ValidateSfnt(fontData);
  (void)ReadUnitsPerEm(fontData);
  (void)HasOutlineTables(fontData);
  return 0;
}

}  // namespace donner::svg
