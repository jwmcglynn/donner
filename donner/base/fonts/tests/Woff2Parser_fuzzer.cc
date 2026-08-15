#include "donner/base/fonts/Woff2Parser.h"

namespace donner::fonts {
namespace {

bool DeclaresLargeOutput(std::span<const uint8_t> data) {
  if (data.size() < 20 || data[0] != 0x77 || data[1] != 0x4F || data[2] != 0x46 ||
      data[3] != 0x32) {
    return false;
  }

  const uint32_t declaredSize =
      (static_cast<uint32_t>(data[16]) << 24) | (static_cast<uint32_t>(data[17]) << 16) |
      (static_cast<uint32_t>(data[18]) << 8) | static_cast<uint32_t>(data[19]);
  return declaredSize >= 32u * 1024u * 1024u;
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> input(data, size);
  // Amplify only the resource dimension encoded by the corpus input. Restoring the former eager
  // output allocation must exceed the per-input watchdog, while the bounded writer remains cheap.
  const size_t iterations = DeclaresLargeOutput(input) ? 8 : 1;
  for (size_t i = 0; i < iterations; ++i) {
    auto result = Woff2Parser::Decompress(input);
    (void)result;
  }

  return 0;
}

}  // namespace donner::fonts
