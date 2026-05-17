#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLTokenizer.h"

namespace donner::xml {
namespace {

class BitStream {
public:
  BitStream(const uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  uint8_t consumeBits(int count) {
    uint8_t value = 0;
    for (int i = 0; i < count; ++i) {
      if (bitOffset_ < size_ * 8) {
        const uint8_t byte = data_[bitOffset_ / 8];
        value |= static_cast<uint8_t>(((byte >> (bitOffset_ % 8)) & 1) << i);
      }
      ++bitOffset_;
    }
    return value;
  }

private:
  const uint8_t* data_;
  std::size_t size_;
  std::size_t bitOffset_ = 0;
};

[[noreturn]] void FuzzInvariantFailed() {
  std::abort();
}

std::string_view PickAttributeName(BitStream& bits) {
  static constexpr std::array<std::string_view, 8> kAttributeNames = {
      "fill", "x", "href", "id", "transform", "d", "xmlns:xlink", "style",
  };
  return kAttributeNames[bits.consumeBits(3) % kAttributeNames.size()];
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }

  const std::size_t controlSize = data[0] % size;
  const uint8_t* controlData = data + 1;
  const std::size_t availableControlSize = std::min<std::size_t>(controlSize, size - 1);
  BitStream bits(controlData, availableControlSize);

  const char* sourceData = reinterpret_cast<const char*>(data + 1 + availableControlSize);
  const std::size_t sourceSize = size - 1 - availableControlSize;
  const std::string_view source(sourceData, sourceSize);

  std::vector<FileOffset> elementStartOffsets;
  std::vector<XMLToken> recordedTokens;
  std::size_t previousEnd = 0;

  Tokenize(source, [&](XMLToken token) {
    if (!token.range.start.offset || !token.range.end.offset) {
      FuzzInvariantFailed();
    }

    const std::size_t start = token.range.start.offset.value();
    const std::size_t end = token.range.end.offset.value();
    if (start > end || start < previousEnd || end > source.size()) {
      FuzzInvariantFailed();
    }
    previousEnd = end;

    const std::string_view text = token.text(source);
    if (text.size() != end - start) {
      FuzzInvariantFailed();
    }

    if (token.type == XMLTokenType::TagOpen && text == "<") {
      elementStartOffsets.push_back(token.range.start);
    }

    const uint8_t action = bits.consumeBits(2);
    if ((action & 1) != 0) {
      recordedTokens.push_back(token);
    }
    if ((action & 2) != 0 && !elementStartOffsets.empty()) {
      const std::size_t index = bits.consumeBits(6) % elementStartOffsets.size();
      const XMLQualifiedNameRef attrName(PickAttributeName(bits));
      (void)XMLParser::GetAttributeLocation(source, elementStartOffsets[index], attrName);
    }
  });

  (void)recordedTokens;
  return 0;
}

}  // namespace donner::xml
