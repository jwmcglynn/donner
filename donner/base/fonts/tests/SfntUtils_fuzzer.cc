#include <cstddef>
#include <cstdint>
#include <span>

#include "donner/base/fonts/SfntUtils.h"

namespace donner::fonts {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> bytes(data, size);
  auto font = SfntFont::Validate(bytes);
  if (font) {
    (void)font->findTable(bytes, "head");
    (void)font->findTable(bytes, "loca");
    (void)font->findTable(bytes, "glyf");
    (void)font->findTable(bytes, "CFF ");
  }
  return 0;
}

}  // namespace donner::fonts
