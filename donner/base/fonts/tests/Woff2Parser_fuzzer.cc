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

void ReplayDeclaredSizeAllocationRegressionSeed() {
  static const auto kResult = [] {
    constexpr std::array<uint8_t, 48> data = {
        0x77, 0x4F, 0x46, 0x32, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00, 0x00, 0x30,
        0x3A, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x08,
        0x08, 0x25, 0x08, 0x08, 0x08, 0x08, 0x08, 0xFA, 0xF7, 0xF7, 0xF7, 0xF7,
        0xF7, 0xF7, 0xF7, 0x08, 0x08, 0x08, 0x08, 0x08, 0x77, 0x0A, 0x4F, 0x32,
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
  ReplayDeclaredSizeAllocationRegressionSeed();

  auto result = Woff2Parser::Decompress(std::span<const uint8_t>(data, size));
  (void)result;

  return 0;
}

}  // namespace donner::fonts
