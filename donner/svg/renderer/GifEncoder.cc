/// @file

#include "donner/svg/renderer/GifEncoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>

namespace donner::svg {
namespace {

constexpr uint32_t kTransparentPixel = 0;
constexpr uint32_t kOpaqueAlpha = 0xFF000000u;
constexpr int kMaxLzwCodes = 4096;
constexpr uint32_t kMaxDelayCentiseconds = std::numeric_limits<uint16_t>::max();
constexpr size_t kGraphicControlBytes = 8;
constexpr size_t kTrailerBytes = 1;
// LZW hash tables use 48 KiB; palette boxes, tables, and vector-growth slack share the remainder.
constexpr size_t kWorkspaceFixedBytes = 128 * 1024;
constexpr size_t kBaseHeaderBytes = 32;
constexpr size_t kTransparentGlobalTableBytes = 6;

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
std::vector<uint32_t> MedianCut(std::vector<ColorCount>& colors, size_t maxColors) {
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
                const int channelDifference = channel(a.rgb, shift) - channel(b.rgb, shift);
                return channelDifference != 0 ? channelDifference < 0 : a.rgb < b.rgb;
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
      if (colors[i].count > dominant->count ||
          (colors[i].count == dominant->count && colors[i].rgb < dominant->rgb)) {
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
  // Reading the final prefix adds the decoder's one lagging dictionary entry. If that entry fills
  // the current width, the following end code must already use the wider code size even though the
  // encoder has no next pair to add to its own dictionary.
  if (codeSize < 12 && maxCode + 1 >= (1u << codeSize)) {
    ++codeSize;
  }
  writer.write(endCode, codeSize);
  writer.finish();
}

/// Whether a span of \p spanSize bytes holds \p height rows of \p width RGBA8 pixels spaced
/// \p stride pixels apart, without the size computation overflowing.
bool PixelSpanFits(size_t spanSize, size_t width, size_t height, size_t stride) {
  constexpr size_t kMaxPixels = std::numeric_limits<size_t>::max() / 4;
  if (stride < width || (height > 1 && stride > (kMaxPixels - width) / (height - 1))) {
    return false;
  }
  return spanSize / 4 >= (height - 1) * stride + width;
}

/**
 * Composite \p rgbaPixels over \p background, or threshold alpha to binary transparency.
 *
 * @return Pixels packed as 0xAARRGGBB with alpha either 0xFF or, for a transparent pixel, all
 *   zero.
 */
std::vector<uint32_t> ResolveAlpha(std::span<const uint8_t> rgbaPixels, size_t width, size_t height,
                                   size_t stride,
                                   const std::optional<GifEncoder::Rgb>& background) {
  std::vector<uint32_t> pixels(width * height);
  for (size_t y = 0; y < height; ++y) {
    const uint8_t* row = rgbaPixels.data() + y * stride * 4;
    for (size_t x = 0; x < width; ++x) {
      const uint8_t* px = row + x * 4;
      uint32_t argb = kTransparentPixel;
      if (background) {
        const uint32_t a = px[3];
        const auto blend = [a](uint8_t src, uint8_t dst) {
          return (uint32_t{src} * a + uint32_t{dst} * (255 - a) + 127) / 255;
        };
        argb = kOpaqueAlpha | (blend(px[0], background->r) << 16) |
               (blend(px[1], background->g) << 8) | blend(px[2], background->b);
      } else if (px[3] >= 128) {
        argb = kOpaqueAlpha | (uint32_t{px[0]} << 16) | (uint32_t{px[1]} << 8) | px[2];
      }
      pixels[y * width + x] = argb;
    }
  }
  return pixels;
}

/// Smallest rectangle containing every pixel that differs between two distinct frames.
PixelRect ChangedRect(const std::vector<uint32_t>& previous, const std::vector<uint32_t>& current,
                      int width, int height) {
  int minX = width;
  int minY = height;
  int maxX = -1;
  int maxY = -1;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t i = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
      if (current[i] != previous[i]) {
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
      }
    }
  }
  return PixelRect{minX, minY, maxX - minX + 1, maxY - minY + 1};
}

/**
 * Rectangle of \p current to store. An opaque animation only needs the rectangle that changed
 * since \p previous drawn over it. With transparency, a pixel may turn transparent, which drawing
 * over the previous frame cannot express, so each frame replaces the whole canvas instead.
 *
 * @param options Encoder configuration.
 * @param previous The previous frame, or nullptr for the first frame.
 * @param current The frame being added, which differs from \p previous.
 */
PixelRect StoredRect(const GifEncoder::Options& options, const std::vector<uint32_t>* previous,
                     const std::vector<uint32_t>& current) {
  if (!options.background || !previous) {
    return PixelRect{0, 0, options.width, options.height};
  }
  return ChangedRect(*previous, current, options.width, options.height);
}

/// Calls \p visit with every pixel of \p rect, in row-major order.
template <typename Visitor>
void ForEachPixel(const std::vector<uint32_t>& pixels, size_t width, const PixelRect& rect,
                  Visitor&& visit) {
  for (int y = rect.top; y < rect.top + rect.height; ++y) {
    for (int x = rect.left; x < rect.left + rect.width; ++x) {
      visit(pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)]);
    }
  }
}

/// A frame's color table and the table index of each of its opaque colors.
struct FramePalette {
  std::vector<uint32_t> colors;
  /// Canonically RGB-sorted colors whose count field holds their index in \ref colors.
  std::vector<ColorCount> indexOf;
};

/// Choose at most \p maxColors table entries for the opaque pixels of \p rect, exactly when they
/// fit and with weighted median cut otherwise.
FramePalette BuildPalette(const std::vector<uint32_t>& pixels, size_t width, const PixelRect& rect,
                          size_t maxColors) {
  FramePalette palette;
  std::vector<ColorCount>& histogram = palette.indexOf;
  histogram.reserve(static_cast<size_t>(rect.width) * static_cast<size_t>(rect.height));
  ForEachPixel(pixels, width, rect, [&histogram](uint32_t argb) {
    if (argb != kTransparentPixel) {
      histogram.push_back({argb & 0xFFFFFFu, 1});
    }
  });
  std::sort(histogram.begin(), histogram.end(),
            [](const ColorCount& a, const ColorCount& b) { return a.rgb < b.rgb; });
  size_t distinct = 0;
  for (const ColorCount color : histogram) {
    if (distinct > 0 && histogram[distinct - 1].rgb == color.rgb) {
      ++histogram[distinct - 1].count;
    } else {
      histogram[distinct++] = color;
    }
  }
  histogram.resize(distinct);

  if (histogram.size() <= maxColors) {
    for (const ColorCount color : histogram) {
      palette.colors.push_back(color.rgb);
    }
  } else {
    palette.colors = MedianCut(histogram, maxColors);
  }

  for (ColorCount& color : histogram) {
    color.count = NearestIndex(palette.colors, color.rgb);
  }
  std::sort(histogram.begin(), histogram.end(),
            [](const ColorCount& a, const ColorCount& b) { return a.rgb < b.rgb; });
  return palette;
}

uint8_t PaletteIndex(const FramePalette& palette, uint32_t rgb) {
  const auto found =
      std::lower_bound(palette.indexOf.begin(), palette.indexOf.end(), rgb,
                       [](const ColorCount& color, uint32_t key) { return color.rgb < key; });
  return static_cast<uint8_t>(found->count);
}

/// Write the image descriptor, local color table and LZW data for \p rect of \p pixels.
std::vector<uint8_t> EncodeImageBlock(const std::vector<uint32_t>& pixels, size_t width,
                                      const PixelRect& rect, const FramePalette& palette,
                                      bool hasTransparency) {
  constexpr uint8_t kTransparentIndex = 0;
  const size_t tableEntries = palette.colors.size() + (hasTransparency ? 1 : 0);
  int tableBits = 1;
  while ((size_t{1} << tableBits) < tableEntries) {
    ++tableBits;
  }

  std::vector<uint8_t> indices;
  indices.reserve(static_cast<size_t>(rect.width) * static_cast<size_t>(rect.height));
  uint32_t lastColor = kTransparentPixel;
  uint8_t lastIndex = kTransparentIndex;
  ForEachPixel(pixels, width, rect, [&](uint32_t argb) {
    if (argb != lastColor) {
      lastColor = argb;
      lastIndex = argb == kTransparentPixel
                      ? kTransparentIndex
                      : static_cast<uint8_t>(PaletteIndex(palette, argb & 0xFFFFFFu) +
                                             (hasTransparency ? 1 : 0));
    }
    indices.push_back(lastIndex);
  });

  std::vector<uint8_t> data;
  data.push_back(0x2C);
  WriteU16(data, static_cast<uint32_t>(rect.left));
  WriteU16(data, static_cast<uint32_t>(rect.top));
  WriteU16(data, static_cast<uint32_t>(rect.width));
  WriteU16(data, static_cast<uint32_t>(rect.height));
  data.push_back(static_cast<uint8_t>(0x80 | (tableBits - 1)));  // Local color table present.
  for (size_t i = 0; i < (size_t{1} << tableBits); ++i) {
    const size_t paletteIndex = hasTransparency && i > 0 ? i - 1 : i;
    const uint32_t rgb = (!hasTransparency || i > 0) && paletteIndex < palette.colors.size()
                             ? palette.colors[paletteIndex]
                             : 0;
    data.push_back(Red(rgb));
    data.push_back(Green(rgb));
    data.push_back(Blue(rgb));
  }
  WriteLzw(data, indices, std::max(2, tableBits));
  return data;
}

bool ResourceLimitsValid(const GifEncoder::Options& options) {
  return options.maximumOutputBytes > 0 &&
         options.maximumOutputBytes <= GifEncoder::kMaximumOutputBytes &&
         options.maximumWorkspaceBytes > 0 &&
         options.maximumWorkspaceBytes <= GifEncoder::kMaximumWorkspaceBytes;
}

bool DimensionsValid(const GifEncoder::Options& options) {
  return options.width > 0 && options.height > 0 && options.width <= 0xFFFF &&
         options.height <= 0xFFFF &&
         static_cast<std::uint64_t>(options.width) * static_cast<std::uint64_t>(options.height) <=
             std::numeric_limits<size_t>::max() / sizeof(uint32_t);
}

size_t HeaderBytes(const GifEncoder::Options& options) {
  return kBaseHeaderBytes + (options.background ? 0 : kTransparentGlobalTableBytes);
}
}  // namespace

std::optional<size_t> GifEncoder::WorkspaceUpperBound(int width, int height) {
  if (width <= 0 || height <= 0 || width > 0xFFFF || height > 0xFFFF) {
    return std::nullopt;
  }
  const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
  // Current and previous resolved pixels (8), sortable ColorCount entries (8), palette indices
  // (1), and retained plus transiently growing encoded blocks (up to 7) total 24 bytes per pixel.
  constexpr uint64_t kBytesPerPixel = 24;
  const uint64_t bytes = pixels * kBytesPerPixel + kWorkspaceFixedBytes;
  if (bytes > std::numeric_limits<size_t>::max()) {
    return std::nullopt;
  }
  return static_cast<size_t>(bytes);
}

GifEncoder::GifEncoder(const Options& options) : options_(options) {
  if (!ResourceLimitsValid(options)) {
    error_ = Error::InvalidConfiguration;
    return;
  }

  // The frame buffers hold width * height 32-bit pixels, which must be addressable.
  if (!DimensionsValid(options)) {
    error_ = Error::InvalidConfiguration;
    return;
  }
  const std::optional<size_t> workspace = WorkspaceUpperBound(options.width, options.height);
  if (!workspace || *workspace > options.maximumWorkspaceBytes) {
    error_ = Error::ResourceLimit;
    valid_ = false;
    return;
  }
  valid_ = true;
  const size_t headerBytes = HeaderBytes(options);
  if (headerBytes > options.maximumOutputBytes - kTrailerBytes) {
    error_ = Error::ResourceLimit;
    valid_ = false;
    return;
  }
  out_.reserve(headerBytes);
  if (out_.capacity() > options.maximumOutputBytes) {
    error_ = Error::ResourceLimit;
    valid_ = false;
    std::vector<uint8_t>().swap(out_);
    return;
  }

  const std::string_view header = "GIF89a";
  out_.insert(out_.end(), header.begin(), header.end());

  // Every frame has a local table; transparent output also defines a minimal global clear table.
  WriteU16(out_, static_cast<uint32_t>(options.width));
  WriteU16(out_, static_cast<uint32_t>(options.height));
  out_.push_back(options.background ? 0x00 : 0x80);  // Transparent GIFs define a 2-entry GCT.
  out_.push_back(0x00);                              // Background color index.
  out_.push_back(0x00);                              // Pixel aspect ratio: unspecified (square).
  if (!options.background) {
    // Compatibility convention: global and local index 0 are consistently reserved for clearing
    // transparent frames. Independent stb_image coverage verifies multi-frame alpha behavior.
    out_.insert(out_.end(), 6, 0);
  }

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
  size_t stride = 0;
  if (!validateFrameInput(rgbaPixels, strideInPixels, &stride)) {
    return false;
  }

  const auto width = static_cast<size_t>(options_.width);
  const auto height = static_cast<size_t>(options_.height);
  std::vector<uint32_t> current =
      ResolveAlpha(rgbaPixels, width, height, stride, options_.background);
  if (pending_ && current == previous_) {
    return extendPendingDelay(delayCentiseconds);
  }

  const PixelRect rect = StoredRect(options_, pending_.has_value() ? &previous_ : nullptr, current);
  const bool transparent = !options_.background.has_value();
  const FramePalette palette = BuildPalette(current, width, rect, transparent ? 255 : 256);

  PendingFrame frame;
  frame.disposal = transparent ? 2 : 1;
  frame.transparent = transparent;
  frame.transparentIndex = 0;
  frame.delayCentiseconds = delayCentiseconds;
  frame.encoded = EncodeImageBlock(current, width, rect, palette, transparent);
  return commitFrame(std::move(frame), std::move(current));
}

bool GifEncoder::validateFrameInput(std::span<const uint8_t> rgbaPixels, size_t strideInPixels,
                                    size_t* stride) {
  if (!valid_ || finished_) {
    if (finished_) {
      error_ = Error::Finished;
    }
    return false;
  }

  const auto width = static_cast<size_t>(options_.width);
  const auto height = static_cast<size_t>(options_.height);
  *stride = strideInPixels == 0 ? width : strideInPixels;
  if (!PixelSpanFits(rgbaPixels.size(), width, height, *stride)) {
    error_ = Error::InvalidFrame;
    return false;
  }
  return true;
}

bool GifEncoder::commitFrame(PendingFrame frame, std::vector<uint32_t> current) {
  const size_t pendingBytes = pending_ ? kGraphicControlBytes + pending_->encoded.size() : 0;
  if (frame.encoded.size() > std::numeric_limits<size_t>::max() - kGraphicControlBytes ||
      pendingBytes >
          std::numeric_limits<size_t>::max() - kGraphicControlBytes - frame.encoded.size() ||
      !outputFits(pendingBytes + kGraphicControlBytes + frame.encoded.size())) {
    return fail(Error::ResourceLimit);
  }
  if (!flushPending()) {
    return false;
  }
  pending_ = std::move(frame);
  previous_ = std::move(current);
  ++storedFrameCount_;
  error_ = Error::None;
  return true;
}

bool GifEncoder::extendPendingDelay(uint16_t delayCentiseconds) {
  const uint32_t total = pending_->delayCentiseconds + delayCentiseconds;
  if (total <= kMaxDelayCentiseconds) {
    pending_->delayCentiseconds = total;
    error_ = Error::None;
    return true;
  }

  // Re-drawing the same frame is idempotent under both disposal methods used here, so the
  // overflow is carried by a repeat of the pending frame.
  PendingFrame repeat = *pending_;
  if (repeat.encoded.size() > (std::numeric_limits<size_t>::max() - 2 * kGraphicControlBytes) / 2 ||
      !outputFits(2 * kGraphicControlBytes + 2 * repeat.encoded.size())) {
    return fail(Error::ResourceLimit);
  }
  pending_->delayCentiseconds = kMaxDelayCentiseconds;
  if (!flushPending()) {
    return false;
  }
  repeat.delayCentiseconds = total - kMaxDelayCentiseconds;
  pending_ = std::move(repeat);
  ++storedFrameCount_;
  error_ = Error::None;
  return true;
}

bool GifEncoder::flushPending() {
  if (!pending_) {
    return true;
  }
  if (pending_->encoded.size() > std::numeric_limits<size_t>::max() - kGraphicControlBytes) {
    return fail(Error::ResourceLimit);
  }
  if (!reserveOutput(kGraphicControlBytes + pending_->encoded.size())) {
    return false;
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
  return true;
}

bool GifEncoder::outputFits(size_t additional) const {
  return additional <= options_.maximumOutputBytes &&
         out_.size() <= options_.maximumOutputBytes - additional &&
         out_.size() + additional <= options_.maximumOutputBytes - kTrailerBytes;
}

bool GifEncoder::reserveOutput(size_t additional) {
  if (!outputFits(additional)) {
    return fail(Error::ResourceLimit);
  }
  return reserveOutputCapacity(out_.size() + additional);
}

bool GifEncoder::reserveOutputCapacity(size_t required) {
  if (required > options_.maximumOutputBytes || out_.capacity() > options_.maximumOutputBytes) {
    return fail(Error::ResourceLimit);
  }
  if (required <= out_.capacity()) {
    return true;
  }

  size_t target = std::max<size_t>(out_.capacity(), 1);
  while (target < required) {
    const size_t remaining = options_.maximumOutputBytes - target;
    if (remaining <= target) {
      target = options_.maximumOutputBytes;
      break;
    }
    target += target;
  }
  if (target < required) {
    return fail(Error::ResourceLimit);
  }

  out_.reserve(target);
  if (out_.capacity() > options_.maximumOutputBytes) {
    return fail(Error::ResourceLimit);
  }
  return true;
}

bool GifEncoder::fail(Error error) {
  error_ = error;
  valid_ = false;
  storedFrameCount_ = 0;
  pending_.reset();
  previous_.clear();
  std::vector<uint8_t>().swap(out_);
  return false;
}

std::vector<uint8_t> GifEncoder::finish() {
  if (!valid_ || finished_ || storedFrameCount_ == 0) {
    finished_ = true;
    pending_.reset();
    out_.clear();
    return {};
  }

  finished_ = true;
  if (!flushPending() || !outputFits(0)) {
    pending_.reset();
    out_.clear();
    return {};
  }
  if (!reserveOutputCapacity(out_.size() + kTrailerBytes)) {
    return {};
  }
  out_.push_back(0x3B);
  error_ = Error::None;
  return std::move(out_);
}

std::vector<uint16_t> GifEncoder::FrameDelaysCentiseconds(double fps, int frameCount) {
  std::vector<uint16_t> delays;
  if (!(fps > 0.0) || frameCount <= 0) {
    return delays;
  }

  // A frame longer than the largest GIF delay is clamped; this also keeps the boundaries below
  // in range for any frame count.
  if (100.0 / fps >= kMaxDelayCentiseconds) {
    return std::vector<uint16_t>(static_cast<size_t>(frameCount),
                                 static_cast<uint16_t>(kMaxDelayCentiseconds));
  }

  delays.reserve(static_cast<size_t>(frameCount));
  const auto boundary = [fps](int frame) {
    return static_cast<std::int64_t>(std::llround(frame * 100.0 / fps));
  };
  for (int i = 0; i < frameCount; ++i) {
    delays.push_back(static_cast<uint16_t>(boundary(i + 1) - boundary(i)));
  }
  return delays;
}

std::ostream& operator<<(std::ostream& out, GifEncoder::Error error) {
  switch (error) {
    case GifEncoder::Error::None: return out << "None";
    case GifEncoder::Error::InvalidConfiguration: return out << "InvalidConfiguration";
    case GifEncoder::Error::InvalidFrame: return out << "InvalidFrame";
    case GifEncoder::Error::ResourceLimit: return out << "ResourceLimit";
    case GifEncoder::Error::Finished: return out << "Finished";
  }
  return out << "Unknown";
}

}  // namespace donner::svg
