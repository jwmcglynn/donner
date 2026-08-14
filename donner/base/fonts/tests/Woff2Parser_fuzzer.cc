#include <array>
#include <cstdlib>

#include "donner/base/fonts/Woff2Parser.h"

namespace donner::fonts {
namespace {

void ReplayInvalidSignatureRegressionSeed() {
  static const auto kResult = [] {
    constexpr std::array<uint8_t, 21> data = {
        0x00, 0xFF, 0xFF, 0xFF, 0xD0, 0xFF, 0xFF, 0x5D, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    return Woff2Parser::Decompress(data);
  }();
  if (!kResult.hasError()) {
    std::abort();
  }
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Keep the regression seed under libFuzzer's per-input watchdog even when mutation starts empty.
  ReplayInvalidSignatureRegressionSeed();

  auto result = Woff2Parser::Decompress(std::span<const uint8_t>(data, size));
  (void)result;

  return 0;
}

}  // namespace donner::fonts
