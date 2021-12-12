#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/css/parser/selector_parser.h"
#include "src/css/selector.h"
#include "src/svg/components/tree_component.h"

using testing::ElementsAre;
using testing::ElementsAreArray;

namespace donner::css {

struct FakeElementData {
  RcString id;
  RcString className;
};

struct FakeElement {
  FakeElement(Registry& registry, Entity entity) : registry_(registry), entity_(entity) {}

  RcString typeString() const { return registry_.get().get<TreeComponent>(entity_).typeString(); }
  RcString id() const { return registry_.get().get_or_emplace<FakeElementData>(entity_).id; }
  RcString className() const {
    return registry_.get().get_or_emplace<FakeElementData>(entity_).className;
  }

  bool hasAttribute(std::string_view name) const {
    // TODO
    return false;
  }

  std::optional<RcString> getAttribute(std::string_view name) const {
    // TODO
    return std::nullopt;
  }

  std::optional<FakeElement> parentElement() {
    auto& tree = registry_.get().get<TreeComponent>(entity_);
    return tree.parent() != entt::null ? std::make_optional(FakeElement(registry_, tree.parent()))
                                       : std::nullopt;
  }

  std::optional<FakeElement> previousSibling() {
    auto& tree = registry_.get().get<TreeComponent>(entity_);
    return tree.previousSibling() != entt::null
               ? std::make_optional(FakeElement(registry_, tree.previousSibling()))
               : std::nullopt;
  }

private:
  std::reference_wrapper<Registry> registry_;
  Entity entity_;
};

class SelectorTests : public testing::Test {
protected:
  Entity createEntity(std::string_view typeString) {
    auto entity = registry_.create();
    registry_.emplace<TreeComponent>(entity, ElementType::Unknown, RcString(typeString), entity);
    return entity;
  }

  bool matches(std::string_view selector, FakeElement element) {
    auto maybeSelector = SelectorParser::Parse(selector);
    EXPECT_THAT(maybeSelector, NoParseError());
    if (maybeSelector.hasError()) {
      return false;
    }

    return maybeSelector.result().matches(element).matched;
  }

  void setId(Entity entity, std::string_view id) {
    registry_.get_or_emplace<FakeElementData>(entity).id = id;
  }

  void setClassName(Entity entity, std::string_view className) {
    registry_.get_or_emplace<FakeElementData>(entity).className = className;
  }

  FakeElement element(Entity entity) { return FakeElement(registry_, entity); }
  TreeComponent& tree(Entity entity) { return registry_.get<TreeComponent>(entity); }

  Registry registry_;
};

TEST_F(SelectorTests, TypeMatch) {
  auto root = createEntity("rect");
  auto child1 = createEntity("a");

  tree(root).appendChild(registry_, child1);

  EXPECT_TRUE(matches("rect", element(root)));
  EXPECT_TRUE(matches("a", element(child1)));
  EXPECT_FALSE(matches("rect", element(child1)));
}

TEST_F(SelectorTests, Combinators) {
  auto root = createEntity("root");
  auto mid = createEntity("mid");
  auto childA = createEntity("a");
  auto childB = createEntity("b");
  auto childC = createEntity("c");
  auto childD = createEntity("d");

  tree(root).appendChild(registry_, mid);
  tree(mid).appendChild(registry_, childA);
  tree(mid).appendChild(registry_, childB);
  tree(mid).appendChild(registry_, childC);
  tree(mid).appendChild(registry_, childD);

  EXPECT_TRUE(matches("root a", element(childA)));
  EXPECT_FALSE(matches("root > a", element(childA)));
  EXPECT_TRUE(matches("root > mid", element(mid)));
  EXPECT_TRUE(matches("a + b", element(childB)));
  EXPECT_FALSE(matches("a + c", element(childC)));
  EXPECT_TRUE(matches("a ~ c", element(childC)));
  EXPECT_TRUE(matches("b ~ c", element(childC)));
  EXPECT_TRUE(matches("root > mid a + b ~ d", element(childD)));
  EXPECT_FALSE(matches("root > mid a + b ~ d", element(childC)));
}

}  // namespace donner::css
