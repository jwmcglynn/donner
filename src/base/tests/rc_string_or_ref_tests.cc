#include <gtest/gtest.h>

#include <unordered_map>

#include "src/base/rc_string_or_ref.h"
#include "src/base/utils.h"

#pragma clang diagnostic ignored "-Wself-assign-overloaded"
#pragma clang diagnostic ignored "-Wself-move"
namespace donner {

TEST(RcStringOrRef, Construct) {
  {
    RcStringOrRef str;
    EXPECT_EQ(str, "");
  }

  {
    RcStringOrRef str("hello");
    EXPECT_EQ(str, "hello");
  }

  {
    RcStringOrRef str(std::string("hello"));
    EXPECT_EQ(str, "hello");
  }

  {
    RcStringOrRef str(std::string_view("world"));
    EXPECT_EQ(str, "world");
  }

  {
    RcStringOrRef str("world\0with\0nulls\0", 17);
    EXPECT_EQ(str, std::string_view("world\0with\0nulls\0", 17));
  }

  {
    RcStringOrRef str("test STRING that is longer than 30 characters");
    EXPECT_EQ(str, std::string_view("test STRING that is longer than 30 characters", 45));
  }

  {
    RcStringOrRef str("test STRING that is longer than 30 characters\0with\0nulls", 56);
    EXPECT_EQ(str,
              std::string_view("test STRING that is longer than 30 characters\0with\0nulls", 56));
  }

  {
    RcStringOrRef str(RcString("test"));
    EXPECT_EQ(str, std::string_view("test"));
  }
}

TEST(RcStringOrRef, Copy) {
  // Copy from same type.
  {
    RcStringOrRef str("hello");

    RcStringOrRef str2(str);  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(str, "hello");
    EXPECT_EQ(str2, "hello");

    RcStringOrRef str3 = str2;  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(str3, "hello");
  }

  // Copy from RcString.
  {
    RcStringOrRef str("hello");
    RcString str2("world");
    str = str2;
    EXPECT_EQ(str, "world");
    EXPECT_EQ(str2, "world");

    RcStringOrRef strCopy(str2);
    EXPECT_EQ(strCopy, "world");
  }

  // Copy from std::string_view.
  {
    RcStringOrRef str("hello");  // value will be replaced
    std::string_view strView("world");
    str = strView;
    EXPECT_EQ(str, "world");

    RcStringOrRef strCopy(strView);
    EXPECT_EQ(strCopy, "world");
  }

  // Test assigning to self
  {
    RcStringOrRef strShort("hello");
    strShort = strShort;
    EXPECT_EQ(strShort, "hello");

    RcStringOrRef strLong(RcString("test STRING that is longer than 30 characters"));
    strLong = strLong;
    EXPECT_EQ(strLong, "test STRING that is longer than 30 characters");
  }
}

TEST(RcStringOrRef, Move) {
  // Move between two string views.
  {
    RcStringOrRef str("hello");
    RcStringOrRef str2(std::move(str));
    EXPECT_EQ(str, "");
    EXPECT_EQ(str2, "hello");
  }

  // Verify that the value being moved out gets destroyed.
  {
    RcStringOrRef str("hello");
    RcStringOrRef str2("world");
    str = std::move(str2);
    EXPECT_EQ(str, "world");
    EXPECT_EQ(str2, "");
  }

  // Move from an RcString.
  {
    RcStringOrRef str(RcString("test STRING that is longer than 30 characters"));
    RcStringOrRef str2(std::move(str));
    EXPECT_EQ(str, "");
    EXPECT_EQ(str2, "test STRING that is longer than 30 characters");
  }

  // Move from an RcString on top of an RcString.
  {
    RcStringOrRef str(RcString("test STRING that is longer than 30 characters"));
    RcStringOrRef str2(RcString("second string that is longer than small string optimization"));
    str = std::move(str2);
    EXPECT_EQ(str, "second string that is longer than small string optimization");
    EXPECT_EQ(str2, "");
  }

  // Move from an RcString over a string view.
  {
    RcStringOrRef str("short");
    RcStringOrRef str2(RcString("test STRING that is longer than 30 characters"));
    str = std::move(str2);
    EXPECT_EQ(str, "test STRING that is longer than 30 characters");
    EXPECT_EQ(str2, "");
  }

  // Move from a string view over an RcString.
  {
    RcStringOrRef str(RcString("test STRING that is longer than 30 characters"));
    RcStringOrRef str2("short");
    str = std::move(str2);
    EXPECT_EQ(str, "short");
    EXPECT_EQ(str2, "");
  }

  // Test moving to self
  {
    RcStringOrRef strShort("hello");
    strShort = std::move(strShort);
    EXPECT_EQ(strShort, "hello");

    RcStringOrRef strLong("test STRING that is longer than 30 characters");
    strLong = std::move(strLong);
    EXPECT_EQ(strLong, "test STRING that is longer than 30 characters");
  }
}

TEST(RcStringOrRef, Assign) {
  // Assign from a `const char*`,
  {
    RcStringOrRef str("hello");
    str = "world";
    EXPECT_EQ(str, "world");
  }

  // Assign from std::string_view.
  {
    RcStringOrRef str("hello");
    str = std::string_view("new world");
    EXPECT_EQ(str, "new world");
  }
}

TEST(RcStringOrRef, CanTransferOwnership) {
  RcString original("test STRING that is longer than 30 characters");
  RcStringOrRef str(original);

  // Test that we can transfer ownership to a new RcString.
  RcString newString = str;
  EXPECT_EQ(newString, "test STRING that is longer than 30 characters");
  EXPECT_EQ(original.data(), newString.data());
}

TEST(RcStringOrRef, Comparison) {
  // operator==
  EXPECT_EQ(RcStringOrRef("hello"), RcStringOrRef("hello"));
  EXPECT_EQ(RcStringOrRef("world"), "world");
  EXPECT_EQ(RcStringOrRef("the"), "the");
  EXPECT_EQ(RcStringOrRef("quick"), std::string_view("quick"));
  EXPECT_EQ(RcStringOrRef("brown"), std::string("brown"));
  EXPECT_TRUE(RcStringOrRef("fox") == RcStringOrRef("fox"));
  EXPECT_TRUE(RcStringOrRef("jumps") == "jumps");
  EXPECT_TRUE(RcStringOrRef("over") == std::string_view("over"));
  EXPECT_TRUE(RcStringOrRef("the") == std::string("the"));
  EXPECT_TRUE("test" == RcStringOrRef("test"));
  EXPECT_TRUE(std::string_view("comparison") == RcStringOrRef("comparison"));
  EXPECT_TRUE(std::string("please") == RcStringOrRef("please"));

  // operator!= (implicitly using spaceship operator)
  EXPECT_NE(RcStringOrRef("ignore"), RcStringOrRef(""));
  EXPECT_NE(RcStringOrRef(""), "empty");
  EXPECT_TRUE(RcStringOrRef("how") != RcStringOrRef("vexingly"));
  EXPECT_TRUE(RcStringOrRef("quick") != "daft");
  EXPECT_TRUE(RcStringOrRef("zebras") != std::string_view("jump"));
  EXPECT_TRUE(RcStringOrRef("zebras") != std::string("jump"));
  EXPECT_TRUE("daft" != RcStringOrRef("quick"));
  EXPECT_TRUE(std::string_view("jump") != RcStringOrRef("zebras"));
  EXPECT_TRUE(std::string("jump") != RcStringOrRef("zebras"));

  // Relative comparisons.
  EXPECT_TRUE(RcStringOrRef("aaa") < RcStringOrRef("bbb"));
  EXPECT_TRUE("ccc" < RcStringOrRef("ddd"));
  EXPECT_TRUE(RcStringOrRef("a") < "b");
}

TEST(RcStringOrRef, Concat) {
  EXPECT_EQ(RcStringOrRef("hello") + RcStringOrRef(" world"), "hello world");
  EXPECT_EQ(RcStringOrRef("the") + " quick", "the quick");
  EXPECT_EQ(RcStringOrRef("brown") + std::string(" fox"), "brown fox");
  EXPECT_EQ("jumps" + RcStringOrRef(" over"), "jumps over");
  EXPECT_EQ(std::string_view("the") + RcStringOrRef(" lazy") + std::string(" dog"), "the lazy dog");
}

TEST(RcStringOrRef, Size) {
  {
    RcStringOrRef str;
    EXPECT_TRUE(str.empty());
    EXPECT_EQ(str.size(), 0);
  }

  {
    RcStringOrRef str("test");
    EXPECT_FALSE(str.empty());
    EXPECT_EQ(str.size(), 4);
  }

  {
    RcStringOrRef str(RcString("test STRING that is longer than 30 characters"));
    EXPECT_FALSE(str.empty());
    EXPECT_EQ(str.size(), 45);
  }
}

TEST(RcStringOrRef, Str) {
  {
    RcStringOrRef str("test");
    EXPECT_EQ(str.str(), "test");
  }

  {
    RcStringOrRef str(RcString("test STRING that is longer than 30 characters"));
    EXPECT_EQ(str.str(), "test STRING that is longer than 30 characters");
  }
}

TEST(RcStringOrRef, Iterators) {
  {
    RcStringOrRef str("test");
    EXPECT_EQ(str.begin(), str.cbegin());
    EXPECT_EQ(str.end(), str.cend());

    EXPECT_EQ(*str.begin(), 't');
    EXPECT_EQ(*(str.end() - 1), 't');
  }

  {
    RcStringOrRef str(RcString("test STRING that is longer than 30 characters"));
    EXPECT_EQ(str.begin(), str.cbegin());
    EXPECT_EQ(str.end(), str.cend());

    EXPECT_EQ(*str.begin(), 't');
    EXPECT_EQ(*(str.end() - 1), 's');
  }
}

TEST(RcStringOrRef, EqualsLowercase) {
  EXPECT_TRUE(RcStringOrRef().equalsLowercase(""));
  EXPECT_TRUE(RcStringOrRef("heLlo").equalsLowercase("hello"));
  EXPECT_TRUE(RcStringOrRef("NONE").equalsLowercase("none"));
  EXPECT_TRUE(RcStringOrRef("test-STRING").equalsLowercase("test-string"));
  EXPECT_TRUE(RcStringOrRef(RcString("test STRING that is longer than 30 characters"))
                  .equalsLowercase("test string that is longer than 30 characters"));

  EXPECT_FALSE(RcStringOrRef("test-STRING").equalsLowercase("string"));
  EXPECT_FALSE(RcStringOrRef("test-STRING").equalsLowercase("test-STRING"))
      << "Should return false since the argument is not lowercase.";
  EXPECT_FALSE(RcStringOrRef("test").equalsLowercase("invalid-length"));
  EXPECT_FALSE(RcStringOrRef(RcString("test STRING that is longer than 30 characters"))
                   .equalsLowercase("other string"));
}

TEST(RcStringOrRef, EqualsIgnoreCase) {
  EXPECT_TRUE(RcStringOrRef().equalsIgnoreCase(""));
  EXPECT_TRUE(RcStringOrRef("heLlo").equalsIgnoreCase("hello"));
  EXPECT_TRUE(RcStringOrRef("none").equalsIgnoreCase("NONE"));
  EXPECT_TRUE(RcStringOrRef("test-STRING").equalsIgnoreCase("TEST-string"));
  EXPECT_TRUE(RcStringOrRef(RcString("test string that is LONGER than 30 characters"))
                  .equalsIgnoreCase("test STRING that is longer than 30 characters"));

  EXPECT_FALSE(RcStringOrRef("test-STRING").equalsIgnoreCase("string"));
  EXPECT_TRUE(RcStringOrRef("test-STRING").equalsIgnoreCase("test-STRING"));
  EXPECT_FALSE(RcStringOrRef("test").equalsIgnoreCase("invalid-length"));
  EXPECT_FALSE(RcStringOrRef(RcString("test STRING that is longer than 30 characters"))
                   .equalsIgnoreCase("other string"));
}

TEST(RcStringOrRef, Output) {
  EXPECT_EQ((std::ostringstream() << RcStringOrRef("")).str(), "");
  EXPECT_EQ((std::ostringstream() << RcStringOrRef("hello world")).str(), "hello world");
}

TEST(RcStringOrRef, HashMap) {
  RcStringOrRef shortKey = "hello";
  RcStringOrRef longKey = RcString("test STRING that is longer than 30 characters");

  std::unordered_map<RcString, int> map;
  map[shortKey] = 1;
  map[longKey] = 2;

  EXPECT_EQ(map["hello"], 1);
  EXPECT_EQ(map["test STRING that is longer than 30 characters"], 2);

  RcString invalidKey = "invalid";
  EXPECT_EQ(map.count(invalidKey), 0);

#if UTILS_EXCEPTIONS_ENABLED()
  EXPECT_THROW(map.at(invalidKey), std::out_of_range);
#endif

  EXPECT_EQ(map.count("invalid"), 0);
}

}  // namespace donner
