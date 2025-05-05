#include "donner/svg/parser/ListParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace donner::svg::parser {

using testing::ElementsAre;
using testing::IsEmpty;

// Helper function to collect parsed items into a vector and check for success
std::vector<std::string> ParseToList(std::string_view str) {
  std::vector<std::string> result;
  std::optional<ParseError> error =
      ListParser::Parse(str, [&](std::string_view item) { result.emplace_back(item); });
  EXPECT_FALSE(error.has_value()) << "Parsing failed unexpectedly for '" << str
                                  << "': " << error.value_or(ParseError{});
  return result;
}

// Helper function to check if parsing fails as expected and optionally check error details
void ExpectParseFailure(std::string_view str,
                        std::optional<std::string_view> expectedReason = std::nullopt,
                        std::optional<size_t> expectedOffset = std::nullopt) {
  std::vector<std::string> result;
  std::optional<ParseError> error = ListParser::Parse(str, [&](std::string_view item) {
    result.emplace_back(item);  // Collect items even on failure for debugging
  });
  EXPECT_TRUE(error.has_value()) << "Parsing succeeded unexpectedly for: " << str;
  if (error.has_value()) {
    if (expectedReason) {
      EXPECT_EQ(error->reason, expectedReason.value());
    }
    if (expectedOffset) {
      EXPECT_EQ(error->location.offset, expectedOffset.value());
    }
  }
}

TEST(ListParser, EmptyString) {
  EXPECT_THAT(ParseToList(""), IsEmpty());
  EXPECT_THAT(ParseToList(" "), IsEmpty());
  EXPECT_THAT(ParseToList("\t\n "), IsEmpty());
}

TEST(ListParser, SingleItem) {
  EXPECT_THAT(ParseToList("item1"), ElementsAre("item1"));
  EXPECT_THAT(ParseToList(" item1"), ElementsAre("item1"));
  EXPECT_THAT(ParseToList("item1 "), ElementsAre("item1"));
  EXPECT_THAT(ParseToList("  item1  "), ElementsAre("item1"));
  EXPECT_THAT(ParseToList("0.0"), ElementsAre("0.0"));
}

TEST(ListParser, CommaSeparated) {
  EXPECT_THAT(ParseToList("item1,item2"), ElementsAre("item1", "item2"));
  EXPECT_THAT(ParseToList("item1, item2"), ElementsAre("item1", "item2"));
  EXPECT_THAT(ParseToList("item1 ,item2"), ElementsAre("item1", "item2"));
  EXPECT_THAT(ParseToList("item1 , item2"), ElementsAre("item1", "item2"));
  EXPECT_THAT(ParseToList("  item1  ,  item2  "), ElementsAre("item1", "item2"));
  EXPECT_THAT(ParseToList("0.0, 0.0"), ElementsAre("0.0", "0.0"));
  EXPECT_THAT(ParseToList("1,2,3"), ElementsAre("1", "2", "3"));
}

TEST(ListParser, SpaceSeparated) {
  EXPECT_THAT(ParseToList("item1 item2"), ElementsAre("item1", "item2"));
  EXPECT_THAT(ParseToList("item1  item2"), ElementsAre("item1", "item2"));
  EXPECT_THAT(ParseToList(" item1 item2 "), ElementsAre("item1", "item2"));
  EXPECT_THAT(ParseToList("1.0 2.0 3.0"), ElementsAre("1.0", "2.0", "3.0"));
  EXPECT_THAT(ParseToList("1.0    2.0    3.0"), ElementsAre("1.0", "2.0", "3.0"));
}

TEST(ListParser, MixedSeparators) {
  EXPECT_THAT(ParseToList("item1,item2 item3"), ElementsAre("item1", "item2", "item3"));
  EXPECT_THAT(ParseToList("item1 item2,item3"), ElementsAre("item1", "item2", "item3"));
  EXPECT_THAT(ParseToList("item1 item2 , item3"), ElementsAre("item1", "item2", "item3"));
  EXPECT_THAT(ParseToList("item1 , item2 item3"), ElementsAre("item1", "item2", "item3"));
  EXPECT_THAT(ParseToList("  item1  ,  item2   item3 "), ElementsAre("item1", "item2", "item3"));
  EXPECT_THAT(ParseToList("1.0 2.0, 3.0"), ElementsAre("1.0", "2.0", "3.0"));
  EXPECT_THAT(ParseToList("1.0, 2.0 3.0"), ElementsAre("1.0", "2.0", "3.0"));
}

TEST(ListParser, InvalidSyntax) {
  // Specific error checks can be added here if desired, e.g.:
  // ExpectParseFailure(",", "Unexpected comma, expected list item", 0);
  ExpectParseFailure(",");                // Just a comma
  ExpectParseFailure(" , ");              // Just a comma with whitespace
  ExpectParseFailure(",item1");           // Leading comma
  ExpectParseFailure(" , item1");         // Leading comma with space
  ExpectParseFailure("item1,");           // Trailing comma
  ExpectParseFailure("item1 , ");         // Trailing comma with space
  ExpectParseFailure("item1,item2,");     // Trailing comma after items
  ExpectParseFailure("item1,,item2");     // Double comma (empty item)
  ExpectParseFailure("item1 , , item2");  // Double comma with spaces
  ExpectParseFailure("item1, ,item2");    // Double comma with spaces
}

}  // namespace donner::svg::parser
