/// @file

#include "donner/svg/renderer/GifEncoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace donner::svg {
namespace {

constexpr uint32_t kTransparentPixel = 0;
constexpr uint32_t kOpaqueAlpha = 0xFF000000u;
constexpr int kMaxLzwCodes = 4096;
constexpr uint32_t kMaxDelayCentiseconds = std::numeric_limits<uint16_t>::max();

void WriteU16(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

constexpr uint8_t Red(uint32_t argb) {
  return static_cast<uint8_t>(argb >> 16);
}
constexpr uint8_t Green(uint32_t argb) {
  return static_cast<uint8_t>(argb >> 8);
}
constexpr uint8_t Blue(uint32_t argb) {
  return static_cast<uint8_t>(argb);
}

/// Pixel rectangle in image coordinates.
struct PixelRect {
  int left = 0;
  int top = 0;
  int width = 0;
  int height = 0;
};

/// One distinct color of a frame and how many pixels use it.
struct ColorCount {
  uint32_t rgb = 0;
  uint32_t count = 0;
};

/**
 * Reduce a color histogram to at most \p maxColors palette entries with weighted median cut.
 *
 * The box to split next is the one with the largest pixel weight times channel range, so a box
 * holding one flat color (range zero) is never split and keeps its color exactly. A box's
 * representative is its most frequent color when that color covers at least half the box's
 * pixels, otherwise the weighted mean; this keeps large flat regions bit-exact even when they
 * share a box with their anti-aliased edge colors.
 */
std::vector<uint32_t> MedianCut(std::vector<ColorCount> colors, size_t maxColors) {
  struct Box {
    size_t begin = 0;
    size_t end = 0;
    uint64_t weight = 0;
    int axisShift = 0;  // Bit shift of the widest channel: 16 red, 8 green, 0 blue.
    int range = 0;
  };

  const auto channel = [](uint32_t rgb, int shift) {
    return static_cast<int>((rgb >> shift) & 0xFF);
  };

  const auto measure = [&](Box& box) {
    std::array<int, 3> lo = {255, 255, 255};
    std::array<int, 3> hi = {0, 0, 0};
    box.weight = 0;
    for (size_t i = box.begin; i < box.end; ++i) {
      for (int c = 0; c < 3; ++c) {
        const int v = channel(colors[i].rgb, 16 - 8 * c);
        lo[c] = std::min(lo[c], v);
        hi[c] = std::max(hi[c], v);
      }
      box.weight += colors[i].count;
    }
    box.range = -1;
    for (int c = 0; c < 3; ++c) {
      if (hi[c] - lo[c] > box.range) {
        box.range = hi[c] - lo[c];
        box.axisShift = 16 - 8 * c;
      }
    }
  };

  std::vector<Box> boxes;
  boxes.push_back(Box{0, colors.size()});
  measure(boxes.back());

  while (boxes.size() < maxColors) {
    size_t best = boxes.size();
    uint64_t bestScore = 0;
    for (size_t i = 0; i < boxes.size(); ++i) {
      const Box& box = boxes[i];
      const uint64_t score =
          box.end - box.begin > 1 ? box.weight * static_cast<uint64_t>(box.range) : 0;
      if (score > bestScore) {
        bestScore = score;
        best = i;
      }
    }
    if (best == boxes.size()) {
      break;
    }

    Box box = boxes[best];
    const int shift = box.axisShift;
    std::sort(colors.begin() + static_cast<std::ptrdiff_t>(box.begin),
              colors.begin() + static_cast<std::ptrdiff_t>(box.end),
              [&](const ColorCount& a, const ColorCount& b) {
                return channel(a.rgb, shift) < channel(b.rgb, shift);
              });

    // Split after the color that reaches half of the box weight, keeping both halves non-empty.
    uint64_t accumulated = 0;
    size_t split = box.begin + 1;
    for (size_t i = box.begin; i + 1 < box.end; ++i) {
      accumulated += colors[i].count;
      split = i + 1;
      if (accumulated * 2 >= box.weight) {
        break;
      }
    }

    Box low{box.begin, split};
    Box high{split, box.end};
    measure(low);
    measure(high);
    boxes[best] = low;
    boxes.push_back(high);
  }

  std::vector<uint32_t> palette;
  palette.reserve(boxes.size());
  for (const Box& box : boxes) {
    const ColorCount* dominant = &colors[box.begin];
    std::array<uint64_t, 3> sums = {0, 0, 0};
    for (size_t i = box.begin; i < box.end; ++i) {
      if (colors[i].count > dominant->count) {
        dominant = &colors[i];
      }
      sums[0] += uint64_t{Red(colors[i].rgb)} * colors[i].count;
      sums[1] += uint64_t{Green(colors[i].rgb)} * colors[i].count;
      sums[2] += uint64_t{Blue(colors[i].rgb)} * colors[i].count;
    }

    if (uint64_t{dominant->count} * 2 >= box.weight) {
      palette.push_back(dominant->rgb);
    } else {
      uint32_t rgb = 0;
      for (int c = 0; c < 3; ++c) {
        const uint64_t mean = (sums[c] + box.weight / 2) / box.weight;
        rgb |= static_cast<uint32_t>(mean) << (16 - 8 * c);
      }
      palette.push_back(rgb);
    }
  }
  return palette;
}

/// Index of the palette entry nearest to \p rgb by squared RGB distance.
uint8_t NearestIndex(const std::vector<uint32_t>& palette, uint32_t rgb) {
  int bestDistance = std::numeric_limits<int>::max();
  size_t best = 0;
  for (size_t i = 0; i < palette.size(); ++i) {
    const int dr = int{Red(palette[i])} - int{Red(rgb)};
    const int dg = int{Green(palette[i])} - int{Green(rgb)};
    const int db = int{Blue(palette[i])} - int{Blue(rgb)};
    const int distance = dr * dr + dg * dg + db * db;
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return static_cast<uint8_t>(best);
}

/// Packs variable-width LZW codes into GIF data sub-blocks of at most 255 bytes.
class CodeWriter {
public:
  explicit CodeWriter(std::vector<uint8_t>& out) : out_(out) {}

  void write(uint32_t code, int bits) {
    bitBuffer_ |= code << bitCount_;
    bitCount_ += bits;
    while (bitCount_ >= 8) {
      pushByte(static_cast<uint8_t>(bitBuffer_ & 0xFF));
      bitBuffer_ >>= 8;
      bitCount_ -= 8;
    }
  }

  /// Flush partial bits and the final sub-block, then write the block terminator.
  void finish() {
    if (bitCount_ > 0) {
      pushByte(static_cast<uint8_t>(bitBuffer_ & 0xFF));
    }
    flushBlock();
    out_.push_back(0);
  }

private:
  void pushByte(uint8_t byte) {
    block_[blockSize_++] = byte;
    if (blockSize_ == block_.size()) {
      flushBlock();
    }
  }

  void flushBlock() {
    if (blockSize_ == 0) {
      return;
    }
    out_.push_back(static_cast<uint8_t>(blockSize_));
    out_.insert(out_.end(), block_.begin(),
                block_.begin() + static_cast<std::ptrdiff_t>(blockSize_));
    blockSize_ = 0;
  }

  std::vector<uint8_t>& out_;
  uint32_t bitBuffer_ = 0;
  int bitCount_ = 0;
  std::array<uint8_t, 255> block_{};
  size_t blockSize_ = 0;
};

/**
 * Write the LZW-compressed image data for \p indices, including the minimum code size byte.
 *
 * The dictionary is an open-addressing hash from (prefix code, next index) to code. Code width
 * grows when the next code to assign no longer fits, matching the decoder's one-entry lag, and the
 * dictionary is reset with a clear code once code 4095 is assigned.
 */
void WriteLzw(std::vector<uint8_t>& out, const std::vector<uint8_t>& indices, int minCodeSize) {
  out.push_back(static_cast<uint8_t>(minCodeSize));

  const uint32_t clearCode = 1u << minCodeSize;
  const uint32_t endCode = clearCode + 1;

  constexpr size_t kTableSize = 1 << 13;
  std::vector<int32_t> keys(kTableSize, -1);
  std::vector<uint16_t> values(kTableSize, 0);

  int codeSize = minCodeSize + 1;
  uint32_t maxCode = endCode;

  CodeWriter writer(out);
  writer.write(clearCode, codeSize);

  uint32_t prefix = indices[0];
  for (size_t i = 1; i < indices.size(); ++i) {
    const uint8_t next = indices[i];
    const int32_t key = static_cast<int32_t>((prefix << 8) | next);
    size_t slot = (static_cast<uint32_t>(key) * 2654435761u) >> (32 - 13);
    bool found = false;
    while (keys[slot] != -1) {
      if (keys[slot] == key) {
        found = true;
        break;
      }
      slot = (slot + 1) & (kTableSize - 1);
    }

    if (found) {
      prefix = values[slot];
      continue;
    }

    writer.write(prefix, codeSize);
    ++maxCode;
    keys[slot] = key;
    values[slot] = static_cast<uint16_t>(maxCode);
    if (maxCode >= (1u << codeSize)) {
      ++codeSize;
    }
    if (maxCode == kMaxLzwCodes - 1) {
      writer.write(clearCode, codeSize);
      std::fill(keys.begin(), keys.end(), -1);
      codeSize = minCodeSize + 1;
      maxCode = endCode;
    }
    prefix = next;
  }

  writer.write(prefix, codeSize);
  writer.write(endCode, codeSize);
  writer.finish();
}

}  // namespace

GifEncoder::GifEncoder(const Options& options) : options_(options) {
  valid_ = options.width > 0 && options.height > 0 && options.width <= 0xFFFF &&
           options.height <= 0xFFFF;
  if (!valid_) {
    return;
  }

  const std::string_view header = "GIF89a";
  out_.insert(out_.end(), header.begin(), header.end());

  // Logical screen descriptor without a global color table; every frame has a local table.
  WriteU16(out_, static_cast<uint32_t>(options.width));
  WriteU16(out_, static_cast<uint32_t>(options.height));
  out_.push_back(0x00);  // Packed fields: no global color table.
  out_.push_back(0x00);  // Background color index.
  out_.push_back(0x00);  // Pixel aspect ratio: unspecified (square).

  // NETSCAPE2.0 application extension: repeat count.
  const std::string_view netscape = "NETSCAPE2.0";
  out_.push_back(0x21);
  out_.push_back(0xFF);
  out_.push_back(static_cast<uint8_t>(netscape.size()));
  out_.insert(out_.end(), netscape.begin(), netscape.end());
  out_.push_back(0x03);
  out_.push_back(0x01);
  WriteU16(out_, options.loopCount);
  out_.push_back(0x00);
}

bool GifEncoder::addFrame(std::span<const uint8_t> rgbaPixels, size_t strideInPixels,
                          uint16_t delayCentiseconds) {
  if (!valid_ || finished_) {
    return false;
  }

  const size_t width = static_cast<size_t>(options_.width);
  const size_t height = static_cast<size_t>(options_.height);
  const size_t stride = strideInPixels == 0 ? width : strideInPixels;
  if (stride < width || rgbaPixels.size() < ((height - 1) * stride + width) * 4) {
    return false;
  }

  // Resolve alpha: composite over the background, or threshold to binary transparency.
  std::vector<uint32_t> current(width * height);
  bool hasTransparency = false;
  for (size_t y = 0; y < height; ++y) {
    const uint8_t* row = rgbaPixels.data() + y * stride * 4;
    for (size_t x = 0; x < width; ++x) {
      const uint8_t* px = row + x * 4;
      uint32_t argb = 0;
      if (options_.background) {
        const uint32_t a = px[3];
        const auto blend = [a](uint8_t src, uint8_t dst) {
          return (uint32_t{src} * a + uint32_t{dst} * (255 - a) + 127) / 255;
        };
        argb = kOpaqueAlpha | (blend(px[0], options_.background->r) << 16) |
               (blend(px[1], options_.background->g) << 8) | blend(px[2], options_.background->b);
      } else if (px[3] >= 128) {
        argb = kOpaqueAlpha | (uint32_t{px[0]} << 16) | (uint32_t{px[1]} << 8) | px[2];
      } else {
        argb = kTransparentPixel;
        hasTransparency = true;
      }
      current[y * width + x] = argb;
    }
  }

  if (pending_ && current == previous_) {
    uint32_t total = pending_->delayCentiseconds + delayCentiseconds;
    if (total > kMaxDelayCentiseconds) {
      // Re-drawing the same frame is idempotent under both disposal methods used here, so the
      // overflow is carried by a repeat of the pending frame.
      PendingFrame repeat = *pending_;
      pending_->delayCentiseconds = kMaxDelayCentiseconds;
      flushPending();
      repeat.delayCentiseconds = total - kMaxDelayCentiseconds;
      pending_ = std::move(repeat);
      ++storedFrameCount_;
    } else {
      pending_->delayCentiseconds = total;
    }
    return true;
  }

  // An opaque animation only needs the changed rectangle drawn over the previous frame. With
  // transparency, a pixel may turn transparent, which drawing over the previous frame cannot
  // express, so each frame replaces the whole canvas instead.
  PixelRect rect{0, 0, options_.width, options_.height};
  const bool cropToChanges = options_.background.has_value() && pending_.has_value();
  if (cropToChanges) {
    int minX = options_.width;
    int minY = options_.height;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < options_.height; ++y) {
      for (int x = 0; x < options_.width; ++x) {
        const size_t i = static_cast<size_t>(y) * width + static_cast<size_t>(x);
        if (current[i] != previous_[i]) {
          minX = std::min(minX, x);
          maxX = std::max(maxX, x);
          minY = std::min(minY, y);
          maxY = std::max(maxY, y);
        }
      }
    }
    rect = PixelRect{minX, minY, maxX - minX + 1, maxY - minY + 1};
  }

  // Histogram of the opaque colors inside the rectangle.
  std::unordered_map<uint32_t, uint32_t> histogram;
  for (int y = rect.top; y < rect.top + rect.height; ++y) {
    for (int x = rect.left; x < rect.left + rect.width; ++x) {
      const uint32_t argb = current[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
      if (argb != kTransparentPixel) {
        ++histogram[argb & 0xFFFFFFu];
      }
    }
  }

  const size_t maxColors = hasTransparency ? 255 : 256;
  std::vector<uint32_t> palette;
  if (histogram.size() <= maxColors) {
    for (const auto& [rgb, count] : histogram) {
      palette.push_back(rgb);
    }
    std::sort(palette.begin(), palette.end());
  } else {
    std::vector<ColorCount> colors;
    colors.reserve(histogram.size());
    for (const auto& [rgb, count] : histogram) {
      colors.push_back({rgb, count});
    }
    palette = MedianCut(std::move(colors), maxColors);
  }

  // Map each distinct color to its palette index once; the histogram is reused as the lookup.
  for (auto& [rgb, index] : histogram) {
    index = NearestIndex(palette, rgb);
  }

  const uint8_t transparentIndex = static_cast<uint8_t>(palette.size());
  const size_t tableEntries = palette.size() + (hasTransparency ? 1 : 0);
  int tableBits = 1;
  while ((size_t{1} << tableBits) < tableEntries) {
    ++tableBits;
  }

  std::vector<uint8_t> indices;
  indices.reserve(static_cast<size_t>(rect.width) * static_cast<size_t>(rect.height));
  uint32_t lastColor = 0;
  uint8_t lastIndex = 0;
  bool haveLast = false;
  for (int y = rect.top; y < rect.top + rect.height; ++y) {
    for (int x = rect.left; x < rect.left + rect.width; ++x) {
      const uint32_t argb = current[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
      if (argb == kTransparentPixel) {
        indices.push_back(transparentIndex);
        continue;
      }
      if (!haveLast || argb != lastColor) {
        lastColor = argb;
        lastIndex = static_cast<uint8_t>(histogram[argb & 0xFFFFFFu]);
        haveLast = true;
      }
      indices.push_back(lastIndex);
    }
  }

  PendingFrame frame;
  frame.disposal = options_.background ? 1 : 2;
  frame.transparent = hasTransparency;
  frame.transparentIndex = hasTransparency ? transparentIndex : 0;
  frame.delayCentiseconds = delayCentiseconds;

  std::vector<uint8_t>& data = frame.encoded;
  data.push_back(0x2C);
  WriteU16(data, static_cast<uint32_t>(rect.left));
  WriteU16(data, static_cast<uint32_t>(rect.top));
  WriteU16(data, static_cast<uint32_t>(rect.width));
  WriteU16(data, static_cast<uint32_t>(rect.height));
  data.push_back(static_cast<uint8_t>(0x80 | (tableBits - 1)));  // Local color table present.
  for (size_t i = 0; i < (size_t{1} << tableBits); ++i) {
    const uint32_t rgb = i < palette.size() ? palette[i] : 0;
    data.push_back(Red(rgb));
    data.push_back(Green(rgb));
    data.push_back(Blue(rgb));
  }
  WriteLzw(data, indices, std::max(2, tableBits));

  flushPending();
  pending_ = std::move(frame);
  previous_ = std::move(current);
  ++storedFrameCount_;
  return true;
}

void GifEncoder::flushPending() {
  if (!pending_) {
    return;
  }

  // Graphic control extension.
  out_.push_back(0x21);
  out_.push_back(0xF9);
  out_.push_back(0x04);
  out_.push_back(static_cast<uint8_t>((pending_->disposal << 2) | (pending_->transparent ? 1 : 0)));
  WriteU16(out_, pending_->delayCentiseconds);
  out_.push_back(pending_->transparentIndex);
  out_.push_back(0x00);

  out_.insert(out_.end(), pending_->encoded.begin(), pending_->encoded.end());
  pending_.reset();
}

std::vector<uint8_t> GifEncoder::finish() {
  if (!valid_ || finished_ || storedFrameCount_ == 0) {
    finished_ = true;
    return {};
  }

  finished_ = true;
  flushPending();
  out_.push_back(0x3B);
  return std::move(out_);
}

std::vector<uint16_t> GifEncoder::FrameDelaysCentiseconds(double fps, int frameCount) {
  std::vector<uint16_t> delays;
  if (!(fps > 0.0) || frameCount <= 0) {
    return delays;
  }

  delays.reserve(static_cast<size_t>(frameCount));
  const auto boundary = [fps](int frame) { return std::llround(frame * 100.0 / fps); };
  for (int i = 0; i < frameCount; ++i) {
    const long long delay = boundary(i + 1) - boundary(i);
    delays.push_back(static_cast<uint16_t>(std::clamp<long long>(delay, 0, kMaxDelayCentiseconds)));
  }
  return delays;
}

}  // namespace donner::svg
