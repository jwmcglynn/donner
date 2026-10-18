#include "donner/svg/renderer/GifEncoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/GifTestDecoder.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "stb/stb_image.h"

using testing::AllOf;
using testing::Each;
using testing::ElementsAre;
using testing::Field;
using testing::Optional;
using testing::SizeIs;

namespace donner::svg {

class GifEncoderTestPeer {
public:
  static size_t outputSize(const GifEncoder& encoder) { return encoder.out_.size(); }
  static size_t outputCapacity(const GifEncoder& encoder) { return encoder.out_.capacity(); }
};

namespace {

/// Straight-alpha RGBA8 test image.
struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;

  Image(int width, int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
      : width(width), height(height) {
    for (int i = 0; i < width * height; ++i) {
      rgba.insert(rgba.end(), {r, g, b, a});
    }
  }

  void set(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    uint8_t* px = &rgba[static_cast<size_t>((y * width + x) * 4)];
    px[0] = r;
    px[1] = g;
    px[2] = b;
    px[3] = a;
  }
};

DecodedGif Decode(const std::vector<uint8_t>& gif) {
  std::string error;
  std::optional<DecodedGif> decoded = DecodeGifForTesting(gif, &error);
  EXPECT_TRUE(decoded.has_value()) << "GIF decode failed: " << error;
  return decoded.value_or(DecodedGif{});
}

/// Wrap a tightly packed straight-alpha RGBA8 buffer as a bitmap for pixelmatch comparison.
RendererBitmap ToBitmap(const std::vector<uint8_t>& rgba, int width, int height) {
  RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(width, height);
  bitmap.pixels = rgba;
  bitmap.rowBytes = static_cast<size_t>(width) * 4;
  bitmap.alphaType = AlphaType::Unpremultiplied;
  return bitmap;
}

/// Expect decoded frame \p index of \p gif to be identical to \p expected, which has the GIF's
/// logical screen size. Mismatches write diff images to the test outputs.
void ExpectFrameIdentical(const DecodedGif& gif, size_t index,
                          const std::vector<uint8_t>& expected) {
  ASSERT_LT(index, gif.frames.size());
  const std::string label =
      std::string(testing::UnitTest::GetInstance()->current_test_info()->name()) + "_frame" +
      std::to_string(index);
  editor::tests::CompareBitmapToBitmap(ToBitmap(gif.frames[index].rgba, gif.width, gif.height),
                                       ToBitmap(expected, gif.width, gif.height), label,
                                       editor::tests::PixelmatchIdentityParams());
}

/// Distinct RGBA colors in a tightly packed RGBA8 buffer.
std::set<std::array<uint8_t, 4>> DistinctColors(const std::vector<uint8_t>& rgba) {
  std::set<std::array<uint8_t, 4>> colors;
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    colors.insert({rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]});
  }
  return colors;
}

/// Encode \p rgba as a single-frame GIF and decode it again.
DecodedGif ReencodeFrame(const std::vector<uint8_t>& rgba, int width, int height) {
  GifEncoder encoder({.width = width, .height = height});
  EXPECT_TRUE(encoder.addFrame(rgba, 0, 1));
  return Decode(encoder.finish());
}

/// A GIF decoded by stb_image, independently of GifTestDecoder.
struct StbDecodedGif {
  int width = 0;
  int height = 0;
  int frameCount = 0;
  std::vector<int> delaysMilliseconds;
  std::vector<uint8_t> rgba;
};

std::optional<StbDecodedGif> DecodeWithStb(std::span<const uint8_t> gif) {
  if (gif.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    ADD_FAILURE() << "GIF is too large for stb_image";
    return std::nullopt;
  }

  int* delays = nullptr;
  StbDecodedGif decoded;
  int components = 0;
  stbi_uc* rgba =
      stbi_load_gif_from_memory(gif.data(), static_cast<int>(gif.size()), &delays, &decoded.width,
                                &decoded.height, &decoded.frameCount, &components, 4);
  if (!rgba) {
    ADD_FAILURE() << "stb_image GIF decode failed: " << stbi_failure_reason();
    return std::nullopt;
  }

  const size_t frameBytes =
      static_cast<size_t>(decoded.width) * static_cast<size_t>(decoded.height) * 4;
  decoded.rgba.assign(rgba, rgba + frameBytes * static_cast<size_t>(decoded.frameCount));
  if (delays) {
    decoded.delaysMilliseconds.assign(delays, delays + decoded.frameCount);
  }
  stbi_image_free(delays);
  stbi_image_free(rgba);
  return decoded;
}

void ExpectStbFrameIdentical(const StbDecodedGif& gif, int index,
                             const std::vector<uint8_t>& expected) {
  ASSERT_GE(index, 0);
  ASSERT_LT(index, gif.frameCount);
  const size_t frameBytes = static_cast<size_t>(gif.width) * static_cast<size_t>(gif.height) * 4;
  ASSERT_EQ(expected.size(), frameBytes);
  const auto begin = gif.rgba.begin() + static_cast<std::ptrdiff_t>(frameBytes * index);
  const std::vector<uint8_t> actual(begin, begin + static_cast<std::ptrdiff_t>(frameBytes));
  const std::string label =
      std::string(testing::UnitTest::GetInstance()->current_test_info()->name()) + "_stb_frame" +
      std::to_string(index);
  editor::tests::CompareBitmapToBitmap(ToBitmap(actual, gif.width, gif.height),
                                       ToBitmap(expected, gif.width, gif.height), label,
                                       editor::tests::PixelmatchIdentityParams());
}

std::vector<uint8_t> FirstLocalColorTable(std::span<const uint8_t> gif) {
  if (gif.size() < 13) {
    ADD_FAILURE() << "GIF is shorter than its logical screen descriptor";
    return {};
  }
  size_t position = 13;
  if ((gif[10] & 0x80) != 0) {
    position += 3 * (size_t{1} << ((gif[10] & 0x07) + 1));
  }
  while (position < gif.size()) {
    if (gif[position] == 0x21) {
      position += 2;
      while (position < gif.size() && gif[position] != 0) {
        position += 1 + gif[position];
      }
      ++position;
      continue;
    }
    if (gif[position] == 0x2C && position + 10 <= gif.size()) {
      const uint8_t packed = gif[position + 9];
      if ((packed & 0x80) == 0) {
        ADD_FAILURE() << "first GIF frame has no local color table";
        return {};
      }
      const size_t bytes = 3 * (size_t{1} << ((packed & 0x07) + 1));
      const size_t begin = position + 10;
      if (bytes > gif.size() - begin) {
        ADD_FAILURE() << "first GIF frame has a truncated local color table";
        return {};
      }
      return std::vector<uint8_t>(gif.begin() + static_cast<std::ptrdiff_t>(begin),
                                  gif.begin() + static_cast<std::ptrdiff_t>(begin + bytes));
    }
    ADD_FAILURE() << "unexpected GIF block before first image";
    return {};
  }
  ADD_FAILURE() << "GIF has no image block";
  return {};
}

uint64_t Fnv1a64(std::span<const uint8_t> bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t byte : bytes) {
    hash = (hash ^ byte) * 1099511628211ull;
  }
  return hash;
}

TEST(GifEncoder, WritesHeaderScreenDescriptorLoopExtensionAndTrailer) {
  GifEncoder encoder({.width = 3, .height = 2});
  const Image frame(3, 2, 10, 20, 30);
  ASSERT_TRUE(encoder.addFrame(frame.rgba, 0, 5));
  const std::vector<uint8_t> gif = encoder.finish();

  ASSERT_THAT(gif, SizeIs(testing::Gt(38u)));
  EXPECT_THAT(std::vector<uint8_t>(gif.begin(), gif.begin() + 38),
              ElementsAre('G', 'I', 'F', '8', '9', 'a',  //
                          3, 0, 2, 0, 0x80, 0x00, 0x00,  // Logical screen, 2-entry global table.
                          0, 0, 0, 0, 0, 0,              // Conventional index-0 clear table.
                          0x21, 0xFF, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 3,
                          1, 0, 0, 0));  // Loop forever.
  EXPECT_EQ(gif.back(), 0x3B);
}

TEST(GifEncoder, LoopCountIsWrittenToNetscapeExtension) {
  GifEncoder encoder({.width = 1, .height = 1, .loopCount = 3});
  const Image frame(1, 1, 0, 0, 0);
  ASSERT_TRUE(encoder.addFrame(frame.rgba, 0, 1));
  EXPECT_THAT(Decode(encoder.finish()).loopCount, Optional(3));
}

TEST(GifEncoder, RoundTripsFramesDelaysAndExactPalette) {
  GifEncoder encoder({.width = 4, .height = 3});
  Image first(4, 3, 255, 0, 0);
  first.set(1, 1, 0, 0, 255);
  Image second(4, 3, 0, 128, 0);
  second.set(3, 2, 250, 250, 250);
  Image third(4, 3, 1, 2, 3);

  ASSERT_TRUE(encoder.addFrame(first.rgba, 0, 5));
  ASSERT_TRUE(encoder.addFrame(second.rgba, 0, 7));
  ASSERT_TRUE(encoder.addFrame(third.rgba, 0, 9));
  EXPECT_EQ(encoder.storedFrameCount(), 3);

  const DecodedGif gif = Decode(encoder.finish());
  EXPECT_EQ(gif.width, 4);
  EXPECT_EQ(gif.height, 3);
  EXPECT_THAT(gif.loopCount, Optional(0));
  ASSERT_THAT(gif.frames, SizeIs(3));
  ExpectFrameIdentical(gif, 0, first.rgba);
  ExpectFrameIdentical(gif, 1, second.rgba);
  ExpectFrameIdentical(gif, 2, third.rgba);
  EXPECT_THAT(gif.frames, ElementsAre(Field(&DecodedGifFrame::delayCentiseconds, 5),
                                      Field(&DecodedGifFrame::delayCentiseconds, 7),
                                      Field(&DecodedGifFrame::delayCentiseconds, 9)));
}

TEST(GifEncoder, WritesSpecConformingLzwData) {
  // Pixels black, black, black, white with a 2-entry table: minimum code size 2, so clear = 4 and
  // end = 5 with 3-bit codes. The codes are clear, 0, 6 (black black), 1, end, packed LSB first:
  // 0b10000100 = 0x84 and 0b01010011 = 0x53. These bytes were checked against an independent
  // decoder.
  GifEncoder encoder({.width = 4, .height = 1, .background = GifEncoder::Rgb{0, 0, 0}});
  Image frame(4, 1, 0, 0, 0);
  frame.set(3, 0, 255, 255, 255);
  ASSERT_TRUE(encoder.addFrame(frame.rgba, 0, 1));
  const std::vector<uint8_t> gif = encoder.finish();

  ASSERT_THAT(gif, SizeIs(testing::Gt(22u)));
  EXPECT_THAT(std::vector<uint8_t>(gif.end() - 22, gif.end()),
              ElementsAre(0x2C, 0, 0, 0, 0, 4, 0, 1, 0, 0x80,  // Image descriptor, 2-entry table.
                          0, 0, 0, 255, 255, 255,              // Color table: black, white.
                          0x02, 0x02, 0x84, 0x53, 0x00,        // LZW data.
                          0x3B));
}

TEST(GifEncoder, OutputCapacityGrowthHasBoundedAmortizedCopyVolume) {
  constexpr int kSize = 64;
  constexpr int kFrames = 512;
  constexpr size_t kOutputLimit = 16 * 1024 * 1024;
  GifEncoder encoder({.width = kSize,
                      .height = kSize,
                      .background = GifEncoder::Rgb{0, 0, 0},
                      .maximumOutputBytes = kOutputLimit});
  std::vector<std::vector<uint8_t>> expectedFrames;
  expectedFrames.reserve(kFrames);

  size_t previousCapacity = GifEncoderTestPeer::outputCapacity(encoder);
  size_t cumulativeCopyBytes = 0;
  for (int frameIndex = 0; frameIndex < kFrames; ++frameIndex) {
    Image frame(kSize, kSize, 0, 0, 0);
    for (int y = 0; y < kSize; ++y) {
      for (int x = 0; x < kSize; ++x) {
        const auto value = static_cast<uint8_t>(x * 37 + y * 71 + frameIndex * 17);
        frame.set(x, y, value, value, value);
      }
    }
    expectedFrames.push_back(frame.rgba);
    ASSERT_TRUE(encoder.addFrame(frame.rgba, 0, 1));

    const size_t capacity = GifEncoderTestPeer::outputCapacity(encoder);
    EXPECT_LE(capacity, kOutputLimit);
    if (capacity != previousCapacity) {
      cumulativeCopyBytes += GifEncoderTestPeer::outputSize(encoder);
      previousCapacity = capacity;
    }
  }

  const std::vector<uint8_t> encoded = encoder.finish();
  ASSERT_FALSE(encoded.empty());
  EXPECT_LE(cumulativeCopyBytes, encoded.size() * 4);
  const DecodedGif decoded = Decode(encoded);
  ASSERT_THAT(decoded.frames, SizeIs(kFrames));
  for (int frameIndex = 0; frameIndex < kFrames; ++frameIndex) {
    ExpectFrameIdentical(decoded, static_cast<size_t>(frameIndex), expectedFrames[frameIndex]);
  }
}

TEST(GifEncoder, RoundTripsWhenFinalPixelCreatesCodeWidthBoundary) {
  constexpr std::string_view kPixels = "101100011111011001";
  Image frame(static_cast<int>(kPixels.size()), 1, 0, 0, 0);
  for (size_t index = 0; index < kPixels.size(); ++index) {
    if (kPixels[index] == '1') {
      frame.set(static_cast<int>(index), 0, 1, 1, 1);
    }
  }

  GifEncoder encoder(
      {.width = frame.width, .height = frame.height, .background = GifEncoder::Rgb{0, 0, 0}});
  ASSERT_TRUE(encoder.addFrame(frame.rgba, 0, 1));

  std::string error;
  const std::optional<DecodedGif> decoded = DecodeGifForTesting(encoder.finish(), &error);
  ASSERT_TRUE(decoded.has_value()) << "GIF decode failed: " << error;
  ASSERT_THAT(decoded->frames, SizeIs(1));
  ExpectFrameIdentical(*decoded, 0, frame.rgba);
}

TEST(GifEncoder, RoundTripsTerminalCodeAtEveryLzwWidthBoundary) {
  // Prefix lengths of a fixed LCG bitstream for which decoding the final pixel grows the
  // dictionary from 3 to 4 bits, then successively through the 11-to-12-bit boundary.
  static constexpr std::array<int, 9> kBoundaryLengths = {4,    19,   66,   202,  533,
                                                          1341, 3201, 7444, 17013};
  std::vector<uint8_t> pixels;
  pixels.reserve(static_cast<size_t>(kBoundaryLengths.back()) * 4);
  uint32_t state = 777;
  for (int index = 0; index < kBoundaryLengths.back(); ++index) {
    state = state * 1664525u + 1013904223u;
    const uint8_t value = static_cast<uint8_t>(state >> 31);
    pixels.insert(pixels.end(), {value, value, value, 255});
  }

  for (size_t boundary = 0; boundary < kBoundaryLengths.size(); ++boundary) {
    const int pixelCount = kBoundaryLengths[boundary];
    const int height = pixelCount == kBoundaryLengths.back() ? 3 : 1;
    const int width = pixelCount / height;
    SCOPED_TRACE(testing::Message() << "terminal width " << boundary + 4);
    GifEncoder encoder({.width = width, .height = height, .background = GifEncoder::Rgb{0, 0, 0}});
    const std::vector<uint8_t> expected(pixels.begin(), pixels.begin() + pixelCount * 4);
    ASSERT_TRUE(encoder.addFrame(expected, 0, 1));

    const DecodedGif decoded = Decode(encoder.finish());
    ASSERT_THAT(decoded.frames, SizeIs(1));
    ExpectFrameIdentical(decoded, 0, expected);
  }
}

TEST(GifEncoder, RespectsRowStride) {
  GifEncoder encoder({.width = 2, .height = 2});
  // Stride of 3 pixels; the padding pixel is never encoded.
  Image padded(3, 2, 9, 9, 9);
  padded.set(0, 0, 1, 1, 1);
  padded.set(1, 1, 2, 2, 2);
  ASSERT_TRUE(encoder.addFrame(padded.rgba, 3, 1));

  const DecodedGif gif = Decode(encoder.finish());
  ASSERT_THAT(gif.frames, SizeIs(1));
  EXPECT_THAT(gif.frames[0].rgba,
              ElementsAre(1, 1, 1, 255, 9, 9, 9, 255, 9, 9, 9, 255, 2, 2, 2, 255));
}

TEST(GifEncoder, MergesIdenticalConsecutiveFrames) {
  GifEncoder encoder({.width = 2, .height = 2});
  const Image a(2, 2, 10, 10, 10);
  const Image b(2, 2, 20, 20, 20);
  ASSERT_TRUE(encoder.addFrame(a.rgba, 0, 5));
  ASSERT_TRUE(encoder.addFrame(a.rgba, 0, 5));
  ASSERT_TRUE(encoder.addFrame(b.rgba, 0, 5));
  ASSERT_TRUE(encoder.addFrame(b.rgba, 0, 4));
  EXPECT_EQ(encoder.storedFrameCount(), 2);

  const DecodedGif gif = Decode(encoder.finish());
  EXPECT_THAT(gif.frames, ElementsAre(Field(&DecodedGifFrame::delayCentiseconds, 10),
                                      Field(&DecodedGifFrame::delayCentiseconds, 9)));
}

TEST(GifEncoder, SplitsMergedDelayThatOverflowsSixteenBits) {
  GifEncoder encoder({.width = 1, .height = 1});
  const Image a(1, 1, 10, 10, 10);
  ASSERT_TRUE(encoder.addFrame(a.rgba, 0, 65000));
  ASSERT_TRUE(encoder.addFrame(a.rgba, 0, 1000));

  const DecodedGif gif = Decode(encoder.finish());
  EXPECT_THAT(gif.frames, ElementsAre(Field(&DecodedGifFrame::delayCentiseconds, 65535),
                                      Field(&DecodedGifFrame::delayCentiseconds, 465)));
  ExpectFrameIdentical(gif, 1, a.rgba);
}

TEST(GifEncoder, OpaqueFramesStoreOnlyTheChangedRectangle) {
  GifEncoder encoder({.width = 8, .height = 6, .background = GifEncoder::Rgb{255, 255, 255}});
  const Image first(8, 6, 0, 0, 255);
  Image second = first;
  second.set(2, 3, 255, 0, 0);
  second.set(4, 4, 0, 255, 0);

  ASSERT_TRUE(encoder.addFrame(first.rgba, 0, 5));
  ASSERT_TRUE(encoder.addFrame(second.rgba, 0, 5));

  const DecodedGif gif = Decode(encoder.finish());
  ASSERT_THAT(gif.frames, SizeIs(2));
  EXPECT_THAT(gif.frames[0],
              AllOf(Field(&DecodedGifFrame::width, 8), Field(&DecodedGifFrame::height, 6)));
  EXPECT_THAT(gif.frames[1],
              AllOf(Field(&DecodedGifFrame::left, 2), Field(&DecodedGifFrame::top, 3),
                    Field(&DecodedGifFrame::width, 3), Field(&DecodedGifFrame::height, 2),
                    Field(&DecodedGifFrame::disposal, 1)));
  ExpectFrameIdentical(gif, 1, second.rgba);
}

TEST(GifEncoder, CompositesOverBackgroundColor) {
  GifEncoder encoder({.width = 2, .height = 1, .background = GifEncoder::Rgb{255, 255, 255}});
  Image frame(2, 1, 255, 0, 0, 128);
  frame.set(1, 0, 0, 0, 0, 0);
  ASSERT_TRUE(encoder.addFrame(frame.rgba, 0, 1));

  const DecodedGif gif = Decode(encoder.finish());
  ASSERT_THAT(gif.frames, SizeIs(1));
  EXPECT_THAT(gif.frames[0].rgba, ElementsAre(255, 127, 127, 255, 255, 255, 255, 255));
}

TEST(GifEncoder, KeepsBinaryTransparencyWithoutBackground) {
  GifEncoder encoder({.width = 2, .height = 1});
  Image first(2, 1, 0, 0, 255);
  first.set(1, 0, 255, 0, 0, 100);  // Below the threshold: transparent.
  Image second(2, 1, 0, 0, 0, 0);
  second.set(1, 0, 255, 0, 0, 200);  // Above the threshold: opaque red.

  ASSERT_TRUE(encoder.addFrame(first.rgba, 0, 2));
  ASSERT_TRUE(encoder.addFrame(second.rgba, 0, 2));

  const std::vector<uint8_t> encoded = encoder.finish();
  ASSERT_GE(encoded.size(), 19u);
  EXPECT_EQ(encoded[10], 0x80);  // Two-entry global color table present.
  EXPECT_EQ(encoded[11], 0);     // Logical background uses the conventional clear index.
  EXPECT_THAT(std::vector<uint8_t>(encoded.begin() + 13, encoded.begin() + 19),
              ElementsAre(0, 0, 0, 0, 0, 0));

  const DecodedGif gif = Decode(encoded);
  ASSERT_THAT(gif.frames, SizeIs(2));
  const std::vector<uint8_t> expectedFirst = {0, 0, 255, 255, 0, 0, 0, 0};
  ExpectFrameIdentical(gif, 0, expectedFirst);
  // The first pixel turned transparent, so the previous frame must not show through.
  EXPECT_THAT(gif.frames[1].rgba, ElementsAre(0, 0, 0, 0, 255, 0, 0, 255));
  EXPECT_THAT(gif.frames, Each(Field(&DecodedGifFrame::disposal, 2)));
  EXPECT_THAT(gif.frames, Each(Field(&DecodedGifFrame::transparentIndex, Optional(0))));

  const std::optional<StbDecodedGif> stb = DecodeWithStb(encoded);
  ASSERT_TRUE(stb.has_value());
  EXPECT_THAT(stb->delaysMilliseconds, ElementsAre(20, 20));
  ExpectStbFrameIdentical(*stb, 0, expectedFirst);
  std::vector<uint8_t> expectedSecond = {0, 0, 0, 0, 255, 0, 0, 255};
  ExpectStbFrameIdentical(*stb, 1, expectedSecond);
}

TEST(GifEncoder, OpaqueFramesOfATransparentGifStillDefineATransparentIndex) {
  // The global background and every local transparent entry conventionally share index 0, so
  // decoders clear an opaque disposed frame before drawing a frame with transparent pixels.
  GifEncoder encoder({.width = 2, .height = 1});
  const Image opaque(2, 1, 0, 0, 255);
  Image transparent = opaque;
  transparent.set(0, 0, 0, 0, 0, 0);
  ASSERT_TRUE(encoder.addFrame(opaque.rgba, 0, 2));
  ASSERT_TRUE(encoder.addFrame(transparent.rgba, 0, 2));

  const DecodedGif gif = Decode(encoder.finish());
  ASSERT_THAT(gif.frames, SizeIs(2));
  EXPECT_THAT(gif.frames[0].transparentIndex, Optional(0));
  EXPECT_THAT(gif.frames[1].transparentIndex, Optional(0));
  EXPECT_THAT(gif.frames[1].rgba, ElementsAre(0, 0, 0, 0, 0, 0, 255, 255));
}

TEST(GifEncoder, QuantizesManyColorsIntoOneFullColorTable) {
  constexpr int kSize = 64;
  Image gradient(kSize, kSize, 0, 0, 0);
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      gradient.set(x, y, static_cast<uint8_t>(x * 4), static_cast<uint8_t>(y * 4),
                   static_cast<uint8_t>((x + y) * 2));
    }
  }

  GifEncoder encoder({.width = kSize, .height = kSize});
  ASSERT_TRUE(encoder.addFrame(gradient.rgba, 0, 1));
  const DecodedGif gif = Decode(encoder.finish());
  ASSERT_THAT(gif.frames, SizeIs(1));
  EXPECT_EQ(gif.frames[0].colorTableSize, 256);
  EXPECT_THAT(DistinctColors(gif.frames[0].rgba),
              SizeIs(testing::AllOf(testing::Gt(128u), testing::Le(256u))));

  // The quantized frame fits one table, so encoding it again is lossless.
  ExpectFrameIdentical(ReencodeFrame(gif.frames[0].rgba, kSize, kSize), 0, gif.frames[0].rgba);
}

TEST(GifEncoder, QuantizedPaletteHasCanonicalFullRgbOrdering) {
  constexpr int kColors = 300;
  Image forward(kColors, 1, 0, 0, 0);
  Image reverse = forward;
  for (int i = 0; i < kColors; ++i) {
    const auto red = static_cast<uint8_t>(i);
    const auto green = static_cast<uint8_t>(i >> 8);
    const auto blue = static_cast<uint8_t>(i * 37);
    forward.set(i, 0, red, green, blue);
    reverse.set(kColors - i - 1, 0, red, green, blue);
  }

  GifEncoder first({.width = kColors, .height = 1});
  GifEncoder second({.width = kColors, .height = 1});
  ASSERT_TRUE(first.addFrame(forward.rgba, 0, 1));
  ASSERT_TRUE(second.addFrame(reverse.rgba, 0, 1));
  const std::vector<uint8_t> firstPalette = FirstLocalColorTable(first.finish());
  const std::vector<uint8_t> secondPalette = FirstLocalColorTable(second.finish());
  EXPECT_EQ(firstPalette, secondPalette);
  // FNV-1a over all 768 canonical table bytes, calculated directly from the documented split
  // rules rather than captured from an encoder result.
  EXPECT_EQ(Fnv1a64(firstPalette), 0xA15DBC4A41D0411Dull);
}

TEST(GifEncoder, QuantizationKeepsFlatColorsExact) {
  // A flat-color diagram whose anti-aliased edge adds more than 256 distinct colors.
  constexpr int kWidth = 400;
  constexpr int kHeight = 40;
  Image diagram(kWidth, kHeight, 30, 60, 200);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = kWidth / 2; x < kWidth; ++x) {
      diagram.set(x, y, 240, 240, 230);
    }
  }
  for (int x = 0; x < kWidth; ++x) {
    const auto t = static_cast<uint8_t>(x * 255 / (kWidth - 1));
    diagram.set(x, kHeight / 2, t, static_cast<uint8_t>(255 - t), static_cast<uint8_t>(t / 2));
  }

  GifEncoder encoder({.width = kWidth, .height = kHeight});
  ASSERT_TRUE(encoder.addFrame(diagram.rgba, 0, 1));
  const DecodedGif gif = Decode(encoder.finish());
  ASSERT_THAT(gif.frames, SizeIs(1));

  // Every row except the many-colored one keeps its flat colors exactly.
  std::vector<uint8_t> expected = diagram.rgba;
  const auto edgeRowBegin = static_cast<std::ptrdiff_t>(kHeight / 2 * kWidth * 4);
  std::copy_n(gif.frames[0].rgba.begin() + edgeRowBegin, kWidth * 4,
              expected.begin() + edgeRowBegin);
  ExpectFrameIdentical(gif, 0, expected);
}

TEST(GifEncoder, RoundTripsAcrossLzwDictionaryResets) {
  constexpr int kSize = 256;
  Image noise(kSize, kSize, 0, 0, 0);
  uint32_t state = 12345;
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      state = state * 1664525u + 1013904223u;
      const auto v = static_cast<uint8_t>(state >> 24);
      noise.set(x, y, v, static_cast<uint8_t>(v ^ 0x5A), static_cast<uint8_t>(255 - v));
    }
  }

  // Opaque, so all 256 colors fit the table exactly.
  GifEncoder encoder({.width = kSize, .height = kSize, .background = GifEncoder::Rgb{0, 0, 0}});
  ASSERT_TRUE(encoder.addFrame(noise.rgba, 0, 1));
  const DecodedGif gif = Decode(encoder.finish());
  ASSERT_THAT(gif.frames, SizeIs(1));
  ExpectFrameIdentical(gif, 0, noise.rgba);
}

TEST(GifEncoder, RoundTripsTwoColorImageAcrossLzwDictionaryResets) {
  // A 1-bit table starts at 3-bit codes, so this covers every code width up to 12 bits and resets.
  constexpr int kSize = 256;
  Image noise(kSize, kSize, 0, 0, 0);
  uint32_t state = 777;
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      state = state * 1664525u + 1013904223u;
      if ((state >> 31) != 0) {
        noise.set(x, y, 255, 255, 255);
      }
    }
  }

  // Opaque, so the table has exactly 2 entries.
  GifEncoder encoder({.width = kSize, .height = kSize, .background = GifEncoder::Rgb{0, 0, 0}});
  ASSERT_TRUE(encoder.addFrame(noise.rgba, 0, 1));
  const std::vector<uint8_t> encoded = encoder.finish();
  const DecodedGif gif = Decode(encoded);
  ASSERT_THAT(gif.frames, SizeIs(1));
  EXPECT_EQ(gif.frames[0].colorTableSize, 2);
  ExpectFrameIdentical(gif, 0, noise.rgba);
  const std::optional<StbDecodedGif> stb = DecodeWithStb(encoded);
  ASSERT_TRUE(stb.has_value());
  ExpectStbFrameIdentical(*stb, 0, noise.rgba);
}

TEST(GifEncoder, QuantizesWithTransparencyReservingTheClearIndex) {
  constexpr int kSize = 32;
  Image gradient(kSize, kSize, 0, 0, 0);
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      gradient.set(x, y, static_cast<uint8_t>(x * 8), static_cast<uint8_t>(y * 8), 100);
    }
  }
  gradient.set(0, 0, 0, 0, 0, 0);

  GifEncoder encoder({.width = kSize, .height = kSize});
  ASSERT_TRUE(encoder.addFrame(gradient.rgba, 0, 1));
  const DecodedGif gif = Decode(encoder.finish());
  ASSERT_THAT(gif.frames, SizeIs(1));
  EXPECT_EQ(gif.frames[0].colorTableSize, 256);
  EXPECT_THAT(gif.frames[0].transparentIndex, Optional(0));
  EXPECT_THAT(std::vector<uint8_t>(gif.frames[0].rgba.begin(), gif.frames[0].rgba.begin() + 4),
              ElementsAre(0, 0, 0, 0));

  // Every other pixel stays opaque: 255 opaque colors plus the transparent one.
  EXPECT_THAT(DistinctColors(gif.frames[0].rgba), SizeIs(testing::Le(256u)));
  std::set<uint8_t> alphas;
  for (size_t i = 3; i < gif.frames[0].rgba.size(); i += 4) {
    alphas.insert(gif.frames[0].rgba[i]);
  }
  EXPECT_THAT(alphas, ElementsAre(0, 255));

  // The quantized frame fits one table, so encoding it again is lossless.
  ExpectFrameIdentical(ReencodeFrame(gif.frames[0].rgba, kSize, kSize), 0, gif.frames[0].rgba);
}

TEST(GifEncoder, QuantizedOpaqueFramesCropToTheChangedRectangle) {
  constexpr int kSize = 32;
  Image gradient(kSize, kSize, 0, 0, 0);
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      gradient.set(x, y, static_cast<uint8_t>(x * 8), static_cast<uint8_t>(y * 8), 100);
    }
  }
  Image changed = gradient;
  changed.set(5, 6, 1, 2, 3);

  GifEncoder encoder({.width = kSize, .height = kSize, .background = GifEncoder::Rgb{0, 0, 0}});
  ASSERT_TRUE(encoder.addFrame(gradient.rgba, 0, 1));
  ASSERT_TRUE(encoder.addFrame(changed.rgba, 0, 1));
  const std::vector<uint8_t> encoded = encoder.finish();
  const DecodedGif gif = Decode(encoded);
  ASSERT_THAT(gif.frames, SizeIs(2));
  EXPECT_THAT(gif.frames[1],
              AllOf(Field(&DecodedGifFrame::left, 5), Field(&DecodedGifFrame::top, 6),
                    Field(&DecodedGifFrame::width, 1), Field(&DecodedGifFrame::height, 1)));
  // Only the changed pixel is redrawn, exactly; the quantized remainder carries over.
  std::vector<uint8_t> expected = gif.frames[0].rgba;
  const size_t changedOffset = (6 * kSize + 5) * 4;
  expected[changedOffset] = 1;
  expected[changedOffset + 1] = 2;
  expected[changedOffset + 2] = 3;
  ExpectFrameIdentical(gif, 1, expected);
  const std::optional<StbDecodedGif> stb = DecodeWithStb(encoded);
  ASSERT_TRUE(stb.has_value());
  ExpectStbFrameIdentical(*stb, 1, expected);
}

TEST(GifEncoder, RejectsEncodedOutputAndWorkspaceAboveConfiguredLimits) {
  const Image frame(4, 4, 10, 20, 30);
  GifEncoder outputLimited({.width = 4, .height = 4, .maximumOutputBytes = 48});
  EXPECT_EQ(outputLimited.error(), GifEncoder::Error::None);
  EXPECT_FALSE(outputLimited.addFrame(frame.rgba, 0, 1));
  EXPECT_EQ(outputLimited.error(), GifEncoder::Error::ResourceLimit);
  EXPECT_THAT(outputLimited.finish(), SizeIs(0));

  GifEncoder workspaceLimited(
      {.width = 4, .height = 4, .maximumOutputBytes = 4096, .maximumWorkspaceBytes = 32});
  EXPECT_FALSE(workspaceLimited.addFrame(frame.rgba, 0, 1));
  EXPECT_EQ(workspaceLimited.error(), GifEncoder::Error::ResourceLimit);
  EXPECT_THAT(workspaceLimited.finish(), SizeIs(0));

  GifEncoder repeatLimited({.width = 1, .height = 1, .maximumOutputBytes = 90});
  const Image repeated(1, 1, 1, 2, 3);
  ASSERT_TRUE(repeatLimited.addFrame(repeated.rgba, 0, 65000));
  EXPECT_FALSE(repeatLimited.addFrame(repeated.rgba, 0, 1000));
  EXPECT_EQ(repeatLimited.error(), GifEncoder::Error::ResourceLimit);
  EXPECT_THAT(repeatLimited.finish(), SizeIs(0));
}

TEST(GifEncoder, RepeatsCroppedFrameWhoseMergedDelayOverflows) {
  GifEncoder encoder({.width = 2, .height = 1, .background = GifEncoder::Rgb{0, 0, 0}});
  const Image first(2, 1, 10, 10, 10);
  Image second = first;
  second.set(1, 0, 20, 20, 20);
  ASSERT_TRUE(encoder.addFrame(first.rgba, 0, 1));
  ASSERT_TRUE(encoder.addFrame(second.rgba, 0, 65000));
  ASSERT_TRUE(encoder.addFrame(second.rgba, 0, 1000));

  const DecodedGif gif = Decode(encoder.finish());
  EXPECT_THAT(gif.frames, ElementsAre(Field(&DecodedGifFrame::delayCentiseconds, 1),
                                      Field(&DecodedGifFrame::delayCentiseconds, 65535),
                                      Field(&DecodedGifFrame::delayCentiseconds, 465)));
  ASSERT_THAT(gif.frames, SizeIs(3));
  ExpectFrameIdentical(gif, 2, second.rgba);
}

TEST(GifEncoder, RejectsInvalidInput) {
  GifEncoder zeroSized({.width = 0, .height = 4});
  const Image tiny(1, 1, 0, 0, 0);
  EXPECT_FALSE(zeroSized.addFrame(tiny.rgba, 0, 1));
  EXPECT_THAT(zeroSized.finish(), SizeIs(0));

  GifEncoder encoder({.width = 2, .height = 2});
  EXPECT_FALSE(encoder.addFrame(tiny.rgba, 0, 1));
  EXPECT_FALSE(encoder.addFrame(Image(2, 2, 0, 0, 0).rgba, 1, 1));
  EXPECT_THAT(encoder.finish(), SizeIs(0));

  GifEncoder tooWide({.width = 65536, .height = 1});
  EXPECT_FALSE(tooWide.addFrame(tiny.rgba, 0, 1));

  GifEncoder outputBypass(
      {.width = 1, .height = 1, .maximumOutputBytes = GifEncoder::kMaximumOutputBytes + 1});
  EXPECT_EQ(outputBypass.error(), GifEncoder::Error::InvalidConfiguration);
  EXPECT_FALSE(outputBypass.addFrame(tiny.rgba, 0, 1));

  // A stride whose size computation would overflow is rejected rather than wrapping.
  GifEncoder strided({.width = 1, .height = 2});
  EXPECT_FALSE(
      strided.addFrame(Image(1, 2, 0, 0, 0).rgba, std::numeric_limits<size_t>::max() / 2, 1));

  GifEncoder finished({.width = 1, .height = 1});
  ASSERT_TRUE(finished.addFrame(tiny.rgba, 0, 1));
  EXPECT_THAT(finished.finish(), SizeIs(testing::Gt(0u)));
  EXPECT_FALSE(finished.addFrame(tiny.rgba, 0, 1));
}

TEST(GifEncoder, FrameDelaysDistributeRoundingWithoutDrift) {
  EXPECT_THAT(GifEncoder::FrameDelaysCentiseconds(20, 4), ElementsAre(5, 5, 5, 5));
  EXPECT_THAT(GifEncoder::FrameDelaysCentiseconds(30, 3), ElementsAre(3, 4, 3));

  const std::vector<uint16_t> delays = GifEncoder::FrameDelaysCentiseconds(30, 30);
  int total = 0;
  for (uint16_t delay : delays) {
    total += delay;
  }
  EXPECT_EQ(total, 100);

  // Above 100 fps some frames round to 0 centiseconds; the total still matches the duration.
  EXPECT_THAT(GifEncoder::FrameDelaysCentiseconds(300, 3), ElementsAre(0, 1, 0));
  // Frames longer than the largest GIF delay are clamped.
  EXPECT_THAT(GifEncoder::FrameDelaysCentiseconds(0.001, 2), ElementsAre(65535, 65535));

  EXPECT_THAT(GifEncoder::FrameDelaysCentiseconds(0, 3), SizeIs(0));
  EXPECT_THAT(GifEncoder::FrameDelaysCentiseconds(10, 0), SizeIs(0));
}

}  // namespace
}  // namespace donner::svg
