#include "donner/base/encoding/Gzip.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"

namespace donner {

using testing::ElementsAreArray;
using testing::HasSubstr;

TEST(Gzip, DecompressSuccess) {
  // gzip-compressed "<svg xmlns='http://www.w3.org/2000/svg'></svg>"
  static const uint8_t kGzipData[] = {
      0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xb3, 0x29, 0x2e,
      0x4b, 0x57, 0xa8, 0xc8, 0xcd, 0xc9, 0x2b, 0xb6, 0x55, 0xcf, 0x28, 0x29, 0x29,
      0xb0, 0xd2, 0xd7, 0x2f, 0x2f, 0x2f, 0xd7, 0x2b, 0x37, 0xd6, 0xcb, 0x2f, 0x4a,
      0xd7, 0x37, 0x32, 0x30, 0x30, 0xd0, 0x07, 0xaa, 0x50, 0xb7, 0xb3, 0x01, 0x51,
      0x76, 0x00, 0xf7, 0xa3, 0x84, 0x65, 0x2e, 0x00, 0x00, 0x00};
  const std::string_view gzipStr(reinterpret_cast<const char*>(kGzipData), sizeof(kGzipData));

  const std::string_view expected = "<svg xmlns='http://www.w3.org/2000/svg'></svg>";
  const std::vector<uint8_t> expectedBytes(expected.begin(), expected.end());
  EXPECT_THAT(DecompressGzip(gzipStr), ParseResultIs(ElementsAreArray(expectedBytes)));
}

TEST(Gzip, DecompressEmpty) {
  EXPECT_THAT(DecompressGzip(""), ParseErrorIs(HasSubstr("Gzip data is too short")));
}

TEST(Gzip, DecompressInvalidHeader) {
  // Not a gzip header.
  static const uint8_t kGzipData[] = {0x00, 0x11, 0x22, 0x33};
  const std::string_view gzipStr(reinterpret_cast<const char*>(kGzipData), sizeof(kGzipData));

  EXPECT_THAT(DecompressGzip(gzipStr), ParseErrorIs(HasSubstr("Invalid gzip header")));
}

TEST(Gzip, DecompressTruncated) {
  // gzip-compressed "<svg xmlns='http://www.w3.org/2000/svg'></svg>", but truncated.
  static const uint8_t kGzipData[] = {
      0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xb3, 0x29, 0x2e,
      0x4b, 0x57, 0xa8, 0xc8, 0xcd, 0xc9, 0x2b, 0xb6, 0x55, 0xcf, 0x28, 0x29, 0x29,
      0xb0, 0xd2, 0xd7, 0x2f, 0x2f, 0x2f, 0xd7, 0x2b, 0x37, 0xd6, 0xcb, 0x2f, 0x4a,
      /* truncated */
  };
  const std::string_view gzipStr(reinterpret_cast<const char*>(kGzipData), sizeof(kGzipData));

  EXPECT_THAT(DecompressGzip(gzipStr), ParseErrorIs(HasSubstr("Failed to decompress")));
}

}  // namespace donner
