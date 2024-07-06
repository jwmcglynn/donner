#include "donner/base/RcString.h"

#include <gtest/gtest.h>

#include <unordered_map>

#include "donner/base/Utils.h"

#pragma clang diagnostic ignored "-Wself-assign-overloaded"
#pragma clang diagnostic ignored "-Wself-move"
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
    RcString str("world\0with\0nulls\0", 17);
    EXPECT_EQ(str, std::string_view("world\0with\0nulls\0", 17));
  }

  {
    RcString str("test STRING that is longer than 30 characters");
    EXPECT_EQ(str, std::string_view("test STRING that is longer than 30 characters", 45));
  }

  {
    RcString str("test STRING that is longer than 30 characters\0with\0nulls", 56);
    EXPECT_EQ(str,
              std::string_view("test STRING that is longer than 30 characters\0with\0nulls", 56));
  }
}

TEST(RcString, ConstructFromVector) {
  std::vector<char> vec = {'h', 'e', 'l', 'l', 'o'};
  RcString str = RcString::fromVector(std::move(vec));

  EXPECT_EQ(str, "hello");
}

TEST(RcString, Copy) {
  // Copy from short.
  {
    RcString str("hello");
    RcString str2(str);  // NOLINT(performance-unnecessary-copy-initialization)
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
    RcString str2(str);  // NOLINT(performance-unnecessary-copy-initialization)
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

  // Test assigning to self
  {
    RcString strShort("hello");
    strShort = strShort;
    EXPECT_EQ(strShort, "hello");

    RcString strLong("test STRING that is longer than 30 characters");
    strLong = strLong;
    EXPECT_EQ(strLong, "test STRING that is longer than 30 characters");
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

  // Test moving to self
  {
    RcString strShort("hello");
    strShort = std::move(strShort);
    EXPECT_EQ(strShort, "hello");

    RcString strLong("test STRING that is longer than 30 characters");
    strLong = std::move(strLong);
    EXPECT_EQ(strLong, "test STRING that is longer than 30 characters");
  }
}

TEST(RcString, Assign) {
  // Assign from a `const char*`,
  {
    RcString str("hello");
    str = "world";
    EXPECT_EQ(str, "world");
  }

  // Assign from std::string_view.
  {
    RcString str("hello");
    str = std::string_view("new world");
    EXPECT_EQ(str, "new world");
  }
}

TEST(RcString, Comparison) {
  // operator==
  EXPECT_EQ(RcString("hello"), RcString("hello"));
  EXPECT_EQ(RcString("world"), "world");
  EXPECT_EQ(RcString("the"), "the");
  EXPECT_EQ(RcString("quick"), std::string_view("quick"));
  EXPECT_EQ(RcString("brown"), std::string("brown"));
  EXPECT_TRUE(RcString("fox") == RcString("fox"));
  EXPECT_TRUE(RcString("jumps") == "jumps");
  EXPECT_TRUE(RcString("over") == std::string_view("over"));
  EXPECT_TRUE(RcString("the") == std::string("the"));
  EXPECT_TRUE("test" == RcString("test"));
  EXPECT_TRUE(std::string_view("comparison") == RcString("comparison"));
  EXPECT_TRUE(std::string("please") == RcString("please"));

  // operator!= (implicitly using spaceship operator)
  EXPECT_NE(RcString("ignore"), RcString(""));
  EXPECT_NE(RcString(""), "empty");
  EXPECT_TRUE(RcString("how") != RcString("vexingly"));
  EXPECT_TRUE(RcString("quick") != "daft");
  EXPECT_TRUE(RcString("zebras") != std::string_view("jump"));
  EXPECT_TRUE(RcString("zebras") != std::string("jump"));
  EXPECT_TRUE("daft" != RcString("quick"));
  EXPECT_TRUE(std::string_view("jump") != RcString("zebras"));
  EXPECT_TRUE(std::string("jump") != RcString("zebras"));

  // Relative comparisons.
  EXPECT_TRUE(RcString("aaa") < RcString("bbb"));
  EXPECT_TRUE("ccc" < RcString("ddd"));
  EXPECT_TRUE(RcString("a") < "b");
}

TEST(RcString, Concat) {
  EXPECT_EQ(RcString("hello") + RcString(" world"), "hello world");
  EXPECT_EQ(RcString("the") + " quick", "the quick");
  EXPECT_EQ(RcString("brown") + std::string(" fox"), "brown fox");
  EXPECT_EQ("jumps" + RcString(" over"), "jumps over");
  EXPECT_EQ(std::string_view("the") + RcString(" lazy") + std::string(" dog"), "the lazy dog");
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

TEST(RcString, Str) {
  {
    RcString str("test");
    EXPECT_EQ(str.str(), "test");
  }

  {
    RcString str("test STRING that is longer than 30 characters");
    EXPECT_EQ(str.str(), "test STRING that is longer than 30 characters");
  }
}

TEST(RcString, Iterators) {
  {
    RcString str("test");
    EXPECT_EQ(str.begin(), str.cbegin());
    EXPECT_EQ(str.end(), str.cend());

    EXPECT_EQ(*str.begin(), 't');
    EXPECT_EQ(*(str.end() - 1), 't');
  }

  {
    RcString str("test STRING that is longer than 30 characters");
    EXPECT_EQ(str.begin(), str.cbegin());
    EXPECT_EQ(str.end(), str.cend());

    EXPECT_EQ(*str.begin(), 't');
    EXPECT_EQ(*(str.end() - 1), 's');
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

TEST(RcString, EqualsIgnoreCase) {
  EXPECT_TRUE(RcString().equalsIgnoreCase(""));
  EXPECT_TRUE(RcString("heLlo").equalsIgnoreCase("hello"));
  EXPECT_TRUE(RcString("none").equalsIgnoreCase("NONE"));
  EXPECT_TRUE(RcString("test-STRING").equalsIgnoreCase("TEST-string"));
  EXPECT_TRUE(RcString("test string that is LONGER than 30 characters")
                  .equalsIgnoreCase("test STRING that is longer than 30 characters"));

  EXPECT_FALSE(RcString("test-STRING").equalsIgnoreCase("string"));
  EXPECT_TRUE(RcString("test-STRING").equalsIgnoreCase("test-STRING"));
  EXPECT_FALSE(RcString("test").equalsIgnoreCase("invalid-length"));
  EXPECT_FALSE(
      RcString("test STRING that is longer than 30 characters").equalsIgnoreCase("other string"));
}

TEST(RcString, Substr) {
  EXPECT_EQ(RcString("hello").substr(0, 0), "");
  EXPECT_EQ(RcString("hello").substr(0), "hello");
  EXPECT_EQ(RcString("world").substr(1), "orld");
  EXPECT_EQ(RcString("world").substr(1, 2), "or");

  EXPECT_EQ(RcString("asdf").substr(0, 10), "asdf")
      << "Should return the maximum number of characters possible";

#if UTILS_EXCEPTIONS_ENABLED()
  EXPECT_THROW(RcString("asdf").substr(10), std::out_of_range) << "Invalid start position.";
#else
  EXPECT_DEATH(RcString("asdf").substr(10), "") << "Invalid start position.";
#endif
}

TEST(RcString, SubstrSmallStringOptimization) {
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

TEST(RcString, HashMap) {
  RcString shortKey = "hello";
  RcString longKey = "test STRING that is longer than 30 characters";

  std::unordered_map<RcString, int> map;
  map[shortKey] = 1;
  map[longKey] = 2;

  EXPECT_EQ(map[shortKey], 1);
  EXPECT_EQ(map[longKey], 2);

  RcString invalidKey = "invalid";
  EXPECT_EQ(map.count(invalidKey), 0);

#if UTILS_EXCEPTIONS_ENABLED()
  EXPECT_THROW(map.at(invalidKey), std::out_of_range);
#endif
}

}  // namespace donner
