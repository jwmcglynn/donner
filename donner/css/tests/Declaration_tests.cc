#include "donner/css/Declaration.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/css/CSS.h"

namespace donner::css {
namespace {

/// Parse inline style and get the first Declaration.
Declaration parseFirst(std::string_view style) {
  auto results = CSS::ParseStyleAttribute(style);
  EXPECT_GE(results.size(), 1u);
  return results[0];
}

TEST(DeclarationSerializeTest, SimpleProperty) {
  const Declaration decl = parseFirst("fill: red");
  EXPECT_EQ(std::string_view(decl.toRcString()), "fill: red");
}

TEST(DeclarationSerializeTest, NumericValue) {
  const Declaration decl = parseFirst("opacity: 0.5");
  EXPECT_EQ(std::string_view(decl.toRcString()), "opacity: 0.5");
}

TEST(DeclarationSerializeTest, ImportantFlag) {
  const Declaration decl = parseFirst("fill: red !important");
  const std::string result(decl.toRcString());
  EXPECT_THAT(result, testing::HasSubstr("fill:"));
  EXPECT_THAT(result, testing::HasSubstr("red"));
  EXPECT_THAT(result, testing::HasSubstr("!important"));
}

TEST(DeclarationSerializeTest, FunctionValue) {
  const Declaration decl = parseFirst("transform: translate(10px, 20px)");
  const std::string result(decl.toRcString());
  EXPECT_THAT(result, testing::HasSubstr("transform:"));
  EXPECT_THAT(result, testing::HasSubstr("translate"));
}

}  // namespace
}  // namespace donner::css
