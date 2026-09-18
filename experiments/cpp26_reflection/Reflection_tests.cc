#include "Reflection.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace donner::experimental {
namespace {

using testing::ElementsAre;

TEST(Reflection, DiscoversMemberNamesInDeclarationOrder) {
  const Rect rect{1, 2, 30, 40};
  std::vector<std::string> names;
  std::vector<double> values;
  ForEachMember(rect, [&](std::string_view name, const double& value) {
    names.emplace_back(name);
    values.push_back(value);
  });
  EXPECT_THAT(names, ElementsAre("x", "y", "width", "height"));
  EXPECT_THAT(values, ElementsAre(1, 2, 30, 40));
}

TEST(Reflection, SplicedMemberAccessMutatesTheOriginalObject) {
  Rect rect{1, 2, 30, 40};
  ForEachMember(rect, [](std::string_view, double& value) { value *= 2; });
  EXPECT_EQ(rect.x, 2);
  EXPECT_EQ(rect.y, 4);
  EXPECT_EQ(rect.width, 60);
  EXPECT_EQ(rect.height, 80);
}

TEST(Reflection, FormatsAnUnregisteredHeterogeneousType) {
  struct Label {
    std::string text;
    int fontSize;
    bool visible;
  };
  EXPECT_EQ(Describe(Label{"Hello", 18, true}), "text=Hello, fontSize=18, visible=1");
}

TEST(Reflection, SupportsTypesWithNoDataMembers) {
  struct Empty {};
  EXPECT_EQ(Describe(Empty{}), "");
}

TEST(Reflection, ReflectsEnumNamesAndRejectsUnknownValues) {
  EXPECT_EQ(EnumName(PaintMode::Fill), "Fill");
  EXPECT_EQ(EnumName(PaintMode::Stroke), "Stroke");
  EXPECT_EQ(EnumName(static_cast<PaintMode>(42)), "<unknown>");
}

}  // namespace
}  // namespace donner::experimental
