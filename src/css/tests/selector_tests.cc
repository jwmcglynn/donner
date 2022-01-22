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
  FakeElement(Registry& registry, TreeComponent* node) : registry_(registry), node_(node) {}

  Entity entity() const { return entt::to_entity(registry_.get(), *node_); }

  RcString typeString() const { return node_->typeString(); }
  RcString id() const { return registry_.get().get_or_emplace<FakeElementData>(entity()).id; }
  RcString className() const {
    return registry_.get().get_or_emplace<FakeElementData>(entity()).className;
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
    return node_->parent() ? std::make_optional(FakeElement(registry_, node_->parent()))
                           : std::nullopt;
  }

  std::optional<FakeElement> previousSibling() {
    return node_->previousSibling()
               ? std::make_optional(FakeElement(registry_, node_->previousSibling()))
               : std::nullopt;
  }

private:
  std::reference_wrapper<Registry> registry_;
  TreeComponent* node_;
};

class SelectorTests : public testing::Test {
protected:
  TreeComponent* createNode(std::string_view typeString) {
    auto entity = registry_.create();
    return &registry_.emplace<TreeComponent>(entity, ElementType::Unknown, RcString(typeString));
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

  FakeElement element(TreeComponent* node) { return FakeElement(registry_, node); }

  Registry registry_;
};

TEST_F(SelectorTests, TypeMatch) {
  auto root = createNode("rect");
  auto child1 = createNode("a");

  root->appendChild(child1);

  EXPECT_TRUE(matches("rect", element(root)));
  EXPECT_TRUE(matches("a", element(child1)));
  EXPECT_FALSE(matches("rect", element(child1)));
}

TEST_F(SelectorTests, Combinators) {
  auto root = createNode("root");
  auto mid = createNode("mid");
  auto childA = createNode("a");
  auto childB = createNode("b");
  auto childC = createNode("c");
  auto childD = createNode("d");

  root->appendChild(mid);
  mid->appendChild(childA);
  mid->appendChild(childB);
  mid->appendChild(childC);
  mid->appendChild(childD);

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
