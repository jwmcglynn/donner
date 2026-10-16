#include "donner/svg/renderer/GifEncoder.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "donner/svg/renderer/tests/GifTestDecoder.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::AllOf;
using testing::Each;
using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::Field;
using testing::Optional;
using testing::SizeIs;

namespace donner::svg {
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

/// Largest absolute per-channel difference between two equally sized RGBA buffers.
int MaxChannelError(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual) {
  int maxError = 0;
  for (size_t i = 0; i < expected.size() && i < actual.size(); ++i) {
    maxError = std::max(maxError, std::abs(int{expected[i]} - int{actual[i]}));
  }
  return maxError;
}

TEST(GifEncoder, WritesHeaderScreenDescriptorLoopExtensionAndTrailer) {
  GifEncoder encoder({.width = 3, .height = 2});
  const Image frame(3, 2, 10, 20, 30);
  ASSERT_TRUE(encoder.addFrame(frame.rgba, 0, 5));
  const std::vector<uint8_t> gif = encoder.finish();

  ASSERT_THAT(gif, SizeIs(testing::Gt(32u)));
  EXPECT_THAT(std::vector<uint8_t>(gif.begin(), gif.begin() + 32),
              ElementsAre('G', 'I', 'F', '8', '9', 'a',  //
                          3, 0, 2, 0, 0x00, 0x00, 0x00,  // Logical screen, no global table.
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
  EXPECT_THAT(gif.frames[0].rgba, ElementsAreArray(first.rgba));
  EXPECT_THAT(gif.frames[1].rgba, ElementsAreArray(second.rgba));
  EXPECT_THAT(gif.frames[2].rgba, ElementsAreArray(third.rgba));
  EXPECT_THAT(gif.frames, ElementsAre(Field(&DecodedGifFrame::delayCentiseconds, 5),
                                      Field(&DecodedGifFrame::delayCentiseconds, 7),
                                      Field(&DecodedGifFrame::delayCentiseconds, 9)));
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
  EXPECT_THAT(gif.frames[1].rgba, ElementsAreArray(a.rgba));
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
  EXPECT_THAT(gif.frames[1].rgba, ElementsAreArray(second.rgba));
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

  const DecodedGif gif = Decode(encoder.finish());
  ASSERT_THAT(gif.frames, SizeIs(2));
  EXPECT_THAT(gif.frames[0].rgba, ElementsAre(0, 0, 255, 255, 0, 0, 0, 0));
  // The first pixel turned transparent, so the previous frame must not show through.
  EXPECT_THAT(gif.frames[1].rgba, ElementsAre(0, 0, 0, 0, 255, 0, 0, 255));
  EXPECT_THAT(gif.frames, Each(Field(&DecodedGifFrame::disposal, 2)));
}

TEST(GifEncoder, QuantizesManyColorsWithBoundedError) {
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
  EXPECT_LE(MaxChannelError(gradient.rgba, gif.frames[0].rgba), 16);
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

  std::vector<uint8_t> expectedFlat;
  std::vector<uint8_t> actualFlat;
  for (int y = 0; y < kHeight; ++y) {
    if (y == kHeight / 2) {
      continue;
    }
    const size_t begin = static_cast<size_t>(y * kWidth * 4);
    const size_t end = begin + static_cast<size_t>(kWidth * 4);
    expectedFlat.insert(expectedFlat.end(), diagram.rgba.begin() + begin,
                        diagram.rgba.begin() + end);
    actualFlat.insert(actualFlat.end(), gif.frames[0].rgba.begin() + begin,
                      gif.frames[0].rgba.begin() + end);
  }
  EXPECT_EQ(MaxChannelError(expectedFlat, actualFlat), 0);
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

  GifEncoder encoder({.width = kSize, .height = kSize});
  ASSERT_TRUE(encoder.addFrame(noise.rgba, 0, 1));
  const DecodedGif gif = Decode(encoder.finish());
  ASSERT_THAT(gif.frames, SizeIs(1));
  EXPECT_THAT(gif.frames[0].rgba, ElementsAreArray(noise.rgba));
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

  EXPECT_THAT(GifEncoder::FrameDelaysCentiseconds(0, 3), SizeIs(0));
  EXPECT_THAT(GifEncoder::FrameDelaysCentiseconds(10, 0), SizeIs(0));
}

}  // namespace
}  // namespace donner::svg
