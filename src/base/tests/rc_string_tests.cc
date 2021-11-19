#include <gtest/gtest.h>

#include "src/base/rc_string.h"

namespace donner {

TEST(RcString, Construct) {
  {
    RcString str;
    EXPECT_EQ(str, "");
  }

  {
    RcString str("hello");
    EXPECT_EQ(str, "hello");
  }

  {
    RcString str(std::string("hello"));
    EXPECT_EQ(str, "hello");
  }

  {
    RcString str(std::string_view("world"));
    EXPECT_EQ(str, "world");
  }

  {
    RcString str("world\0with\0nulls\0");
    EXPECT_EQ(str, std::string_view("world\0with\0nulls\0", 17));
  }
}

TEST(RcString, Copy) {
  {
    RcString str("hello");
    RcString str2(str);
    EXPECT_EQ(str, "hello");
    EXPECT_EQ(str2, "hello");
  }

  {
    RcString str("hello");
    RcString str2("world");
    str = str2;
    EXPECT_EQ(str, "world");
    EXPECT_EQ(str2, "world");
  }
}

TEST(RcString, Move) {
  {
    RcString str;
    RcString str2(std::move(str));
    EXPECT_EQ(str, "");
    EXPECT_EQ(str2, "");
  }

  {
    RcString str("hello");
    RcString str2(std::move(str));
    EXPECT_EQ(str, "");
    EXPECT_EQ(str2, "hello");
  }

  {
    RcString str("world");
    RcString str2;
    str2 = std::move(str);
    EXPECT_EQ(str, "");
    EXPECT_EQ(str2, "world");
  }
}

TEST(RcString, Size) {
  {
    RcString str;
    EXPECT_TRUE(str.empty());
    EXPECT_EQ(str.size(), 0);
  }

  {
    RcString str("test");
    EXPECT_FALSE(str.empty());
    EXPECT_EQ(str.size(), 4);
  }
}

TEST(RcString, EqualsLowercase) {
  EXPECT_TRUE(RcString().equalsLowercase(""));
  EXPECT_TRUE(RcString("heLlo").equalsLowercase("hello"));
  EXPECT_TRUE(RcString("NONE").equalsLowercase("none"));
  EXPECT_TRUE(RcString("test-STRING").equalsLowercase("test-string"));

  EXPECT_FALSE(RcString("test-STRING").equalsLowercase("string"));
  EXPECT_FALSE(RcString("test-STRING").equalsLowercase("test-STRING"))
      << "Should return false since the argument is not lowercase.";
  EXPECT_FALSE(RcString("test").equalsLowercase("invalid-length"));
}

TEST(RcString, Substr) {
  EXPECT_EQ(RcString("hello").substr(0, 0), "");
  EXPECT_EQ(RcString("hello").substr(0), "hello");
  EXPECT_EQ(RcString("world").substr(1), "orld");
  EXPECT_EQ(RcString("world").substr(1, 2), "or");

  EXPECT_EQ(RcString("asdf").substr(0, 10), "asdf")
      << "Should return the maximum number of characters possible";
  EXPECT_THROW(RcString("asdf").substr(10), std::out_of_range) << "Invalid start position.";
}

TEST(RcString, Duplicate) {
  EXPECT_EQ(RcString("").substr(0).duplicate(), "");
  EXPECT_EQ(RcString("hello world").substr(0, 5).duplicate(), "hello");

  {
    const RcString original("test string");
    const RcString substr = original.substr(0, 4);
    EXPECT_EQ(original.data(), substr.data());

    const RcString duplicate = substr.duplicate();
    EXPECT_NE(original.data(), duplicate.data());
  }
}

TEST(RcString, Output) {
  EXPECT_EQ((std::ostringstream() << RcString("")).str(), "");
  EXPECT_EQ((std::ostringstream() << RcString("hello world")).str(), "hello world");
}

}  // namespace donner