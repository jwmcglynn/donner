#include "donner/base/encoding/Decompress.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"

namespace donner {

using testing::HasSubstr;

TEST(DecompressTest, Gzip) {
  // "hello world" compressed with gzip
  const std::vector<uint8_t> compressed = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x03, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57,
                                           0x28, 0xcf, 0x2f, 0xca, 0x49, 0x01, 0x00, 0x85,
                                           0x11, 0x4a, 0x0d, 0x0b, 0x00, 0x00, 0x00};
  auto maybeResult = Decompress::Gzip(
      std::string_view(reinterpret_cast<const char*>(compressed.data()), compressed.size()));
  ASSERT_THAT(maybeResult, NoParseError());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(maybeResult.result().data()),
                        maybeResult.result().size()),
            "hello world");
}

TEST(DecompressTest, Zlib) {
  // "hello world" compressed with zlib
  const std::vector<uint8_t> compressed = {0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9,
                                           0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0x01,
                                           0x00, 0x1a, 0x0b, 0x04, 0x5d};
  auto maybeResult = Decompress::Zlib(
      std::string_view(reinterpret_cast<const char*>(compressed.data()), compressed.size()), 11);
  ASSERT_THAT(maybeResult, NoParseError());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(maybeResult.result().data()),
                        maybeResult.result().size()),
            "hello world");
}

TEST(DecompressTest, GzipInvalidHeader) {
  const std::vector<uint8_t> compressed = {0x00, 0x00};
  auto maybeResult = Decompress::Gzip(
      std::string_view(reinterpret_cast<const char*>(compressed.data()), compressed.size()));
  EXPECT_THAT(maybeResult, ParseErrorIs("Invalid gzip header"));
}

TEST(DecompressTest, GzipTooShort) {
  const std::vector<uint8_t> compressed = {0x1f};
  auto maybeResult = Decompress::Gzip(
      std::string_view(reinterpret_cast<const char*>(compressed.data()), compressed.size()));
  EXPECT_THAT(maybeResult, ParseErrorIs("Gzip data is too short"));
}

TEST(DecompressTest, ZlibInvalidData) {
  const std::vector<uint8_t> compressed = {0x00, 0x00};
  auto maybeResult = Decompress::Zlib(
      std::string_view(reinterpret_cast<const char*>(compressed.data()), compressed.size()), 11);
  EXPECT_THAT(maybeResult, ParseErrorIs(HasSubstr("Failed to decompress zlib data")));
}

TEST(DecompressTest, ZlibSizeMismatch) {
  // "hello world" compressed with zlib
  const std::vector<uint8_t> compressed = {0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9,
                                           0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0x01,
                                           0x00, 0x1a, 0x0b, 0x04, 0x5d};
  auto maybeResult = Decompress::Zlib(
      std::string_view(reinterpret_cast<const char*>(compressed.data()), compressed.size()), 12);
  EXPECT_THAT(maybeResult, ParseErrorIs("Zlib decompression size mismatch"));
}

}  // namespace donner
