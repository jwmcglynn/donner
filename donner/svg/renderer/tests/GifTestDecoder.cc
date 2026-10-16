/// @file

#include "donner/svg/renderer/tests/GifTestDecoder.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace donner::svg {
namespace {

/// Bounds-checked little-endian byte reader.
class Reader {
public:
  explicit Reader(std::span<const uint8_t> data) : data_(data) {}

  bool has(size_t count) const { return pos_ + count <= data_.size(); }
  uint8_t u8() { return has(1) ? data_[pos_++] : (failed_ = true, 0); }
  int u16() {
    const int lo = u8();
    const int hi = u8();
    return lo | (hi << 8);
  }
  std::span<const uint8_t> bytes(size_t count) {
    if (!has(count)) {
      failed_ = true;
      return {};
    }
    auto result = data_.subspan(pos_, count);
    pos_ += count;
    return result;
  }
  bool failed() const { return failed_; }

  /// Concatenate a sequence of data sub-blocks up to the zero-length terminator.
  std::vector<uint8_t> subBlocks() {
    std::vector<uint8_t> result;
    while (!failed_) {
      const uint8_t size = u8();
      if (size == 0) {
        break;
      }
      auto block = bytes(size);
      result.insert(result.end(), block.begin(), block.end());
    }
    return result;
  }

private:
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
  bool failed_ = false;
};

/// Decode GIF LZW data into color indices.
bool DecodeLzw(int minCodeSize, const std::vector<uint8_t>& data, size_t pixelCount,
               std::vector<uint8_t>* out, std::string* error) {
  if (minCodeSize < 2 || minCodeSize > 8) {
    *error = "invalid LZW minimum code size";
    return false;
  }

  const int clearCode = 1 << minCodeSize;
  const int endCode = clearCode + 1;
  std::vector<std::vector<uint8_t>> dict;
  const auto reset = [&] {
    dict.assign(static_cast<size_t>(endCode + 1), {});
    for (int i = 0; i < clearCode; ++i) {
      dict[static_cast<size_t>(i)] = {static_cast<uint8_t>(i)};
    }
  };
  reset();

  int codeSize = minCodeSize + 1;
  int previous = -1;
  size_t bitPos = 0;
  bool sawEnd = false;
  while (bitPos + static_cast<size_t>(codeSize) <= data.size() * 8) {
    int code = 0;
    for (int b = 0; b < codeSize; ++b, ++bitPos) {
      code |= ((data[bitPos / 8] >> (bitPos % 8)) & 1) << b;
    }

    if (code == clearCode) {
      reset();
      codeSize = minCodeSize + 1;
      previous = -1;
      continue;
    }
    if (code == endCode) {
      sawEnd = true;
      break;
    }

    std::vector<uint8_t> entry;
    if (previous == -1) {
      if (code >= clearCode) {
        *error = "first code after clear is not a literal";
        return false;
      }
      entry = dict[static_cast<size_t>(code)];
    } else {
      if (code < static_cast<int>(dict.size())) {
        entry = dict[static_cast<size_t>(code)];
        if (dict.size() < 4096) {
          std::vector<uint8_t> added = dict[static_cast<size_t>(previous)];
          added.push_back(entry[0]);
          dict.push_back(std::move(added));
        }
      } else if (code == static_cast<int>(dict.size())) {
        entry = dict[static_cast<size_t>(previous)];
        entry.push_back(entry[0]);
        dict.push_back(entry);
      } else {
        *error = "LZW code out of range";
        return false;
      }
      if (static_cast<int>(dict.size()) == (1 << codeSize) && codeSize < 12) {
        ++codeSize;
      }
    }

    out->insert(out->end(), entry.begin(), entry.end());
    previous = code;
  }

  if (!sawEnd) {
    *error = "missing LZW end code";
    return false;
  }
  if (out->size() != pixelCount) {
    *error = "decoded " + std::to_string(out->size()) + " pixels, expected " +
             std::to_string(pixelCount);
    return false;
  }
  return true;
}

}  // namespace

std::optional<DecodedGif> DecodeGifForTesting(std::span<const uint8_t> data, std::string* error) {
  std::string ignored;
  if (!error) {
    error = &ignored;
  }

  Reader reader(data);
  auto header = reader.bytes(6);
  if (reader.failed() ||
      std::string_view(reinterpret_cast<const char*>(header.data()), 6) != "GIF89a") {
    *error = "missing GIF89a header";
    return std::nullopt;
  }

  DecodedGif gif;
  gif.width = reader.u16();
  gif.height = reader.u16();
  const uint8_t screenFlags = reader.u8();
  reader.u8();  // Background color index.
  reader.u8();  // Pixel aspect ratio.

  std::vector<uint8_t> globalTable;
  if (screenFlags & 0x80) {
    auto table = reader.bytes(size_t{3} << ((screenFlags & 0x07) + 1));
    globalTable.assign(table.begin(), table.end());
  }

  const size_t canvasBytes = static_cast<size_t>(gif.width) * static_cast<size_t>(gif.height) * 4;
  std::vector<uint8_t> canvas(canvasBytes, 0);
  int disposal = 0;
  int delay = 0;
  bool transparent = false;
  int transparentIndex = 0;

  while (!reader.failed()) {
    const uint8_t introducer = reader.u8();
    if (introducer == 0x3B) {
      return gif;
    }

    if (introducer == 0x21) {
      const uint8_t label = reader.u8();
      std::vector<uint8_t> payload = reader.subBlocks();
      if (label == 0xF9 && payload.size() == 4) {
        disposal = (payload[0] >> 2) & 0x07;
        transparent = (payload[0] & 0x01) != 0;
        delay = payload[1] | (payload[2] << 8);
        transparentIndex = payload[3];
      } else if (label == 0xFF && payload.size() == 14 &&
                 std::string_view(reinterpret_cast<const char*>(payload.data()), 11) ==
                     "NETSCAPE2.0" &&
                 payload[11] == 0x01) {
        gif.loopCount = payload[12] | (payload[13] << 8);
      }
      continue;
    }

    if (introducer != 0x2C) {
      *error = "unexpected block introducer " + std::to_string(introducer);
      return std::nullopt;
    }

    DecodedGifFrame frame;
    frame.left = reader.u16();
    frame.top = reader.u16();
    frame.width = reader.u16();
    frame.height = reader.u16();
    frame.disposal = disposal;
    frame.delayCentiseconds = delay;
    const uint8_t imageFlags = reader.u8();
    if (imageFlags & 0x40) {
      *error = "interlaced images are not supported";
      return std::nullopt;
    }
    if (disposal == 3) {
      *error = "disposal 3 is not supported";
      return std::nullopt;
    }
    if (frame.left + frame.width > gif.width || frame.top + frame.height > gif.height) {
      *error = "frame exceeds the logical screen";
      return std::nullopt;
    }

    std::vector<uint8_t> table = globalTable;
    if (imageFlags & 0x80) {
      auto local = reader.bytes(size_t{3} << ((imageFlags & 0x07) + 1));
      table.assign(local.begin(), local.end());
    }
    frame.colorTableSize = static_cast<int>(table.size() / 3);
    if (table.empty()) {
      *error = "frame has no color table";
      return std::nullopt;
    }

    const int minCodeSize = reader.u8();
    const std::vector<uint8_t> lzw = reader.subBlocks();
    if (reader.failed()) {
      break;
    }

    std::vector<uint8_t> indices;
    const size_t pixelCount = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    if (!DecodeLzw(minCodeSize, lzw, pixelCount, &indices, error)) {
      return std::nullopt;
    }

    for (int y = 0; y < frame.height; ++y) {
      for (int x = 0; x < frame.width; ++x) {
        const int index = indices[static_cast<size_t>(y * frame.width + x)];
        if (transparent && index == transparentIndex) {
          continue;
        }
        if (index >= frame.colorTableSize) {
          *error = "color index outside the color table";
          return std::nullopt;
        }
        uint8_t* px = &canvas[(static_cast<size_t>(frame.top + y) * static_cast<size_t>(gif.width) +
                               static_cast<size_t>(frame.left + x)) *
                              4];
        px[0] = table[static_cast<size_t>(index) * 3];
        px[1] = table[static_cast<size_t>(index) * 3 + 1];
        px[2] = table[static_cast<size_t>(index) * 3 + 2];
        px[3] = 255;
      }
    }

    frame.rgba = canvas;
    gif.frames.push_back(std::move(frame));

    const DecodedGifFrame& drawn = gif.frames.back();
    if (disposal == 2) {
      for (int y = drawn.top; y < drawn.top + drawn.height; ++y) {
        std::fill_n(canvas.begin() + static_cast<std::ptrdiff_t>(
                                         (static_cast<size_t>(y) * static_cast<size_t>(gif.width) +
                                          static_cast<size_t>(drawn.left)) *
                                         4),
                    drawn.width * 4, uint8_t{0});
      }
    }
    disposal = 0;
    delay = 0;
    transparent = false;
  }

  *error = "truncated GIF";
  return std::nullopt;
}

}  // namespace donner::svg
