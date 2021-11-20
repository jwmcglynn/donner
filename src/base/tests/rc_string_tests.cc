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

  {
    RcString str("test STRING that is longer than 30 characters");
    EXPECT_EQ(str, std::string_view("test STRING that is longer than 30 characters", 45));
  }

  {
    RcString str("test STRING that is longer than 30 characters\0with\0nulls");
    EXPECT_EQ(str,
              std::string_view("test STRING that is longer than 30 characters\0with\0nulls", 56));
  }
}

TEST(RcString, Copy) {
  // Copy from short.
  {
    RcString str("hello");
    RcString str2(str);
    EXPECT_EQ(str, "hello");
    EXPECT_EQ(str2, "hello");
  }

  // Copy from short to short.
  {
    RcString str("hello");
    RcString str2("world");
    str = str2;
    EXPECT_EQ(str, "world");
    EXPECT_EQ(str2, "world");
  }

  // Copy from long.
  {
    RcString str("test STRING that is longer than 30 characters");
    RcString str2(str);
    EXPECT_EQ(str, "test STRING that is longer than 30 characters");
    EXPECT_EQ(str2, "test STRING that is longer than 30 characters");
  }

  // Copy from long to long.
  {
    RcString str("test STRING that is longer than 30 characters");
    RcString str2("second string that is longer than small string optimization");
    str = str2;
    EXPECT_EQ(str, "second string that is longer than small string optimization");
    EXPECT_EQ(str2, "second string that is longer than small string optimization");
  }

  // Copy from long to short.
  {
    RcString str("short");
    RcString str2("test STRING that is longer than 30 characters");
    str = str2;
    EXPECT_EQ(str, "test STRING that is longer than 30 characters");
    EXPECT_EQ(str2, "test STRING that is longer than 30 characters");
  }

  // Copy from short to long.
  {
    RcString str("test STRING that is longer than 30 characters");
    RcString str2("short");
    str = str2;
    EXPECT_EQ(str, "short");
    EXPECT_EQ(str2, "short");
  }
}

TEST(RcString, Move) {
  // Move from short.
  {
    RcString str("hello");
    RcString str2(std::move(str));
    EXPECT_EQ(str, "");
    EXPECT_EQ(str2, "hello");
  }

  // Move from short to short.
  {
    RcString str("hello");
    RcString str2("world");
    str = std::move(str2);
    EXPECT_EQ(str, "world");
    EXPECT_EQ(str2, "");
  }

  // Move from long.
  {
    RcString str("test STRING that is longer than 30 characters");
    RcString str2(std::move(str));
    EXPECT_EQ(str, "");
    EXPECT_EQ(str2, "test STRING that is longer than 30 characters");
  }

  // Move from long to long.
  {
    RcString str("test STRING that is longer than 30 characters");
    RcString str2("second string that is longer than small string optimization");
    str = std::move(str2);
    EXPECT_EQ(str, "second string that is longer than small string optimization");
    EXPECT_EQ(str2, "");
  }

  // Move from long to short.
  {
    RcString str("short");
    RcString str2("test STRING that is longer than 30 characters");
    str = std::move(str2);
    EXPECT_EQ(str, "test STRING that is longer than 30 characters");
    EXPECT_EQ(str2, "");
  }

  // Move from short to long.
  {
    RcString str("test STRING that is longer than 30 characters");
    RcString str2("short");
    str = std::move(str2);
    EXPECT_EQ(str, "short");
    EXPECT_EQ(str2, "");
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

  {
    RcString str("test STRING that is longer than 30 characters");
    EXPECT_FALSE(str.empty());
    EXPECT_EQ(str.size(), 45);
  }
}

TEST(RcString, EqualsLowercase) {
  EXPECT_TRUE(RcString().equalsLowercase(""));
  EXPECT_TRUE(RcString("heLlo").equalsLowercase("hello"));
  EXPECT_TRUE(RcString("NONE").equalsLowercase("none"));
  EXPECT_TRUE(RcString("test-STRING").equalsLowercase("test-string"));
  EXPECT_TRUE(RcString("test STRING that is longer than 30 characters")
                  .equalsLowercase("test string that is longer than 30 characters"));

  EXPECT_FALSE(RcString("test-STRING").equalsLowercase("string"));
  EXPECT_FALSE(RcString("test-STRING").equalsLowercase("test-STRING"))
      << "Should return false since the argument is not lowercase.";
  EXPECT_FALSE(RcString("test").equalsLowercase("invalid-length"));
  EXPECT_FALSE(
      RcString("test STRING that is longer than 30 characters").equalsLowercase("other string"));
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

TEST(RcString, Substr_SmallStringOptimization) {
  // If the substr range is large enough, the substr will contain a reference.
  {
    const RcString original("test string that is longer than 30 characters");
    const RcString substr = original.substr(0, original.size() - 1);
    EXPECT_EQ(original.data(), substr.data());
  }

  {
    const RcString original("test string that is longer than 30 characters");
    const RcString substr = original.substr(0, 4);
    EXPECT_NE(original.data(), substr.data());
  }
}

TEST(RcString, Dedup) {
  {
    RcString str = RcString("").substr(0);
    str.dedup();
    EXPECT_EQ(str, "");
  }

  {
    RcString str = RcString("hello world").substr(0, 5);
    str.dedup();
    EXPECT_EQ(str, "hello");
  }

  {
    const RcString original("test string that is longer than 30 characters");
    const RcString substr = original.substr(0, original.size() - 1);
    EXPECT_EQ(original.data(), substr.data());

    RcString duplicate = substr;
    duplicate.dedup();
    EXPECT_NE(original.data(), duplicate.data());
  }
}

TEST(RcString, Output) {
  EXPECT_EQ((std::ostringstream() << RcString("")).str(), "");
  EXPECT_EQ((std::ostringstream() << RcString("hello world")).str(), "hello world");
}

}  // namespace donner