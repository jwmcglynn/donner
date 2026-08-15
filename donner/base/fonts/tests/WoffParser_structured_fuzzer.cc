#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "donner/base/fonts/WoffParser.h"

namespace donner::fonts {
namespace {

void AppendBe16(std::vector<uint8_t>& output, uint16_t value) {
  output.push_back(static_cast<uint8_t>(value >> 8));
  output.push_back(static_cast<uint8_t>(value));
}

void AppendBe32(std::vector<uint8_t>& output, uint32_t value) {
  output.push_back(static_cast<uint8_t>(value >> 24));
  output.push_back(static_cast<uint8_t>(value >> 16));
  output.push_back(static_cast<uint8_t>(value >> 8));
  output.push_back(static_cast<uint8_t>(value));
}

void WriteBe32(std::vector<uint8_t>& output, size_t offset, uint32_t value) {
  output[offset] = static_cast<uint8_t>(value >> 24);
  output[offset + 1] = static_cast<uint8_t>(value >> 16);
  output[offset + 2] = static_cast<uint8_t>(value >> 8);
  output[offset + 3] = static_cast<uint8_t>(value);
}

struct Table {
  uint32_t tag;
  std::vector<uint8_t> data;
};

std::vector<uint8_t> BuildWoff(FuzzedDataProvider& provider, size_t maximumSfntSize) {
  const size_t tableCount = provider.ConsumeIntegralInRange<size_t>(0, 8);
  std::vector<Table> tables;
  tables.reserve(tableCount);
  for (size_t i = 0; i < tableCount; ++i) {
    tables.push_back(
        {provider.ConsumeIntegral<uint32_t>(),
         provider.ConsumeBytes<uint8_t>(provider.ConsumeIntegralInRange<size_t>(0, 64))});
  }

  std::vector<uint8_t> output;
  output.reserve(44 + tableCount * 20 + provider.remaining_bytes());
  AppendBe32(output, 0x774F4646);  // wOFF
  AppendBe32(output, 0x00010000);  // TrueType flavor
  AppendBe32(output, 0);           // Patched with file length below.
  AppendBe16(output, static_cast<uint16_t>(tableCount));
  AppendBe16(output, 0);

  size_t reconstructedSize = 12 + tableCount * 16;
  for (const Table& table : tables) {
    reconstructedSize += (table.data.size() + 3) & ~size_t{3};
  }
  const uint32_t declaredSfntSize = static_cast<uint32_t>(
      provider.ConsumeBool() ? reconstructedSize : std::min(reconstructedSize, maximumSfntSize));
  AppendBe32(output, declaredSfntSize);
  AppendBe16(output, 1);
  AppendBe16(output, 0);
  for (int i = 0; i < 5; ++i) {
    AppendBe32(output, 0);  // Metadata and private-data offsets/lengths.
  }

  size_t dataOffset = 44 + tableCount * 20;
  for (const Table& table : tables) {
    AppendBe32(output, table.tag);
    AppendBe32(output, static_cast<uint32_t>(dataOffset));
    AppendBe32(output, static_cast<uint32_t>(table.data.size()));
    AppendBe32(output, static_cast<uint32_t>(table.data.size()));
    AppendBe32(output, provider.ConsumeIntegral<uint32_t>());
    dataOffset += table.data.size();
  }
  for (const Table& table : tables) {
    output.insert(output.end(), table.data.begin(), table.data.end());
  }
  WriteBe32(output, 8, static_cast<uint32_t>(output.size()));
  return output;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider provider(data, size);
  WoffParser::Options options;
  options.maximumInputSize = provider.ConsumeIntegralInRange<size_t>(0, 512);
  options.maximumTableSize = provider.ConsumeIntegralInRange<size_t>(0, 128);
  options.maximumSfntSize = provider.ConsumeIntegralInRange<size_t>(12, 512);
  const std::vector<uint8_t> woff = BuildWoff(provider, options.maximumSfntSize);

  auto result = WoffParser::Parse(woff, options);
  if (woff.size() > options.maximumInputSize && result.hasResult()) {
    std::abort();
  }
  if (result.hasResult()) {
    size_t reconstructedSize = 12 + result.result().tables.size() * 16;
    for (const WoffTable& table : result.result().tables) {
      if (table.data.size() > options.maximumTableSize) {
        std::abort();
      }
      reconstructedSize += (table.data.size() + 3) & ~size_t{3};
    }
    if (reconstructedSize > options.maximumSfntSize) {
      std::abort();
    }
  }
  return 0;
}

}  // namespace donner::fonts
