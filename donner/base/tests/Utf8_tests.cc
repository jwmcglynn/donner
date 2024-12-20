#include "donner/base/Utf8.h"

#include <gtest/gtest.h>

namespace donner::base {

TEST(CommonUtf8, SequenceLength) {
  for (int i = 0; i < 0x80; ++i) {
    EXPECT_EQ(Utf8SequenceLength(char(i)), 1) << "i = " << i;
  }

  // Codepoints starting with 0b10 are not valid.
  for (int i = 0x80; i < 0xC0; ++i) {
    EXPECT_EQ(Utf8SequenceLength(char(i)), 0) << "i = " << i;
  }

  for (int i = 0xC0; i < 0xE0; ++i) {
    EXPECT_EQ(Utf8SequenceLength(char(i)), 2) << "i = " << i;
  }

  for (int i = 0xE0; i < 0xF0; ++i) {
    EXPECT_EQ(Utf8SequenceLength(char(i)), 3) << "i = " << i;
  }

  for (int i = 0xF0; i < 0xF8; ++i) {
    EXPECT_EQ(Utf8SequenceLength(char(i)), 4) << "i = " << i;
  }

  // Values after 4 bytes are not valid.
  for (int i = 0xF8; i < 0xFF; ++i) {
    EXPECT_EQ(Utf8SequenceLength(char(i)), 0) << "i = " << i;
  }
}

}  // namespace donner::base
