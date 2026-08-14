#include <array>
#include <cstdlib>
#include <vector>

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
  const auto result = [] {
    std::array<uint8_t, 50> data{};
    data[0] = 0x77;
    data[1] = 0x4F;
    data[2] = 0x46;
    data[3] = 0x32;
    data[5] = 1;
    data[11] = data.size();
    data[13] = 1;
    data[16] = 2;
    data[28] = 8;
    data[29] = 8;
    data[30] = 8;
    data[31] = 8;
    return Woff2Parser::Decompress(data);
  }();
  if (!result.hasError() || result.error().reason != "WOFF2: decompression failed") {
    std::abort();
  }
}

void ReplayLinuxTimeoutRegressionSeed() {
  static const auto kResult = [] {
    constexpr std::array<uint8_t, 48> data = {
        0x77, 0x4F, 0x46, 0x32, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00, 0x00, 0x30,
        0x3A, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x08,
        0x08, 0x25, 0x08, 0x08, 0x08, 0x08, 0x08, 0xFA, 0xF7, 0xF7, 0xF7, 0xF7,
        0xF7, 0xF7, 0xF7, 0x08, 0x08, 0x08, 0x08, 0x08, 0x77, 0x0A, 0x4F, 0x32,
    };
    return Woff2Parser::Decompress(data);
  }();
  if (!kResult.hasError() || kResult.error().reason != "WOFF2: table count exceeds limit") {
    std::abort();
  }
}

void ReplayIntermediateAllocationRegressionSeed() {
  static const auto kResult = [] {
    constexpr size_t kInputSize = 64u * 1024u;
    std::vector<uint8_t> data(kInputSize, 0);
    data[0] = 0x77;
    data[1] = 0x4F;
    data[2] = 0x46;
    data[3] = 0x32;
    data[5] = 1;
    data[8] = static_cast<uint8_t>(kInputSize >> 24);
    data[9] = static_cast<uint8_t>(kInputSize >> 16);
    data[10] = static_cast<uint8_t>(kInputSize >> 8);
    data[11] = static_cast<uint8_t>(kInputSize);
    data[13] = 1;
    data[16] = 4;

    size_t directoryEnd = 48;
    data[directoryEnd++] = 0;
    std::array<uint8_t, 4> encodedLength = {0x88, 0x80, 0x80, 0x01};
    for (uint8_t byte : encodedLength) {
      data[directoryEnd++] = byte;
    }
    const uint32_t compressedLength = static_cast<uint32_t>(data.size() - directoryEnd);
    data[20] = static_cast<uint8_t>(compressedLength >> 24);
    data[21] = static_cast<uint8_t>(compressedLength >> 16);
    data[22] = static_cast<uint8_t>(compressedLength >> 8);
    data[23] = static_cast<uint8_t>(compressedLength);

    return Woff2Parser::Decompress(data);
  }();
  if (!kResult.hasError() ||
      kResult.error().reason != "WOFF2: intermediate decompressed size exceeds limit") {
    std::abort();
  }
}

void ReplayTransformedGlyfAllocationRegressionSeed() {
  static const auto kResult = [] {
    constexpr size_t kInputSize = 64u * 1024u;
    std::vector<uint8_t> data(kInputSize, 0);
    data[0] = 0x77;
    data[1] = 0x4F;
    data[2] = 0x46;
    data[3] = 0x32;
    data[5] = 1;
    data[8] = static_cast<uint8_t>(kInputSize >> 24);
    data[9] = static_cast<uint8_t>(kInputSize >> 16);
    data[10] = static_cast<uint8_t>(kInputSize >> 8);
    data[11] = static_cast<uint8_t>(kInputSize);
    data[13] = 1;
    data[16] = 4;

    size_t directoryEnd = 48;
    data[directoryEnd++] = 10;
    constexpr std::array<uint8_t, 4> kOversizedGlyfLength = {0x82, 0x80, 0x80, 0x01};
    for (size_t i = 0; i < 2; ++i) {
      for (uint8_t byte : kOversizedGlyfLength) {
        data[directoryEnd++] = byte;
      }
    }
    const uint32_t compressedLength = static_cast<uint32_t>(data.size() - directoryEnd);
    data[20] = static_cast<uint8_t>(compressedLength >> 24);
    data[21] = static_cast<uint8_t>(compressedLength >> 16);
    data[22] = static_cast<uint8_t>(compressedLength >> 8);
    data[23] = static_cast<uint8_t>(compressedLength);

    return Woff2Parser::Decompress(data);
  }();
  if (!kResult.hasError() ||
      kResult.error().reason != "WOFF2: transformed glyf size exceeds limit") {
    std::abort();
  }
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Keep the regression seed under libFuzzer's per-input watchdog even when mutation starts empty.
  ReplayInvalidSignatureRegressionSeed();
  ReplayDeclaredSizeAllocationRegressionSeed();
  ReplayLinuxTimeoutRegressionSeed();
  ReplayIntermediateAllocationRegressionSeed();
  ReplayTransformedGlyfAllocationRegressionSeed();

  auto result = Woff2Parser::Decompress(std::span<const uint8_t>(data, size));
  (void)result;

  return 0;
}

}  // namespace donner::fonts
