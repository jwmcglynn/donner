#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>

#include "donner/base/RcString.h"
#include "donner/base/parser/tests/ParseResultTestUtils.h"
#include "donner/css/Selector.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/svg/components/AttributesComponent.h"
#include "donner/svg/components/TreeComponent.h"

using testing::ElementsAre;
using testing::ElementsAreArray;

namespace donner::css::parser {

using namespace base::parser;  // NOLINT: For tests

struct FakeElementData {
  RcString id;
  RcString className;
};

struct FakeElement {
  FakeElement(svg::Registry& registry, svg::Entity entity) : registry_(registry), entity_(entity) {}

  bool operator==(const FakeElement& other) const { return entity_ == other.entity_; }

  svg::XMLQualifiedNameRef xmlTypeName() const {
    return registry_.get().get<svg::components::TreeComponent>(entity_).xmlTypeName();
  }
  RcString id() const { return registry_.get().get_or_emplace<FakeElementData>(entity_).id; }
  RcString className() const {
    return registry_.get().get_or_emplace<FakeElementData>(entity_).className;
  }

  std::optional<RcString> getAttribute(const svg::XMLQualifiedNameRef& name) const {
    return registry_.get()
        .get_or_emplace<svg::components::AttributesComponent>(entity_)
        .getAttribute(name);
  }

  std::vector<svg::XMLQualifiedNameRef> findMatchingAttributes(
      const svg::XMLQualifiedNameRef& matcher) const {
    return registry_.get()
        .get_or_emplace<svg::components::AttributesComponent>(entity_)
        .findMatchingAttributes(matcher);
  }

  std::optional<FakeElement> parentElement() const {
    auto& tree = registry_.get().get<svg::components::TreeComponent>(entity_);
    return tree.parent() != entt::null ? std::make_optional(FakeElement(registry_, tree.parent()))
                                       : std::nullopt;
  }

  std::optional<FakeElement> firstChild() const {
    auto& tree = registry_.get().get<svg::components::TreeComponent>(entity_);
    return tree.firstChild() != entt::null
               ? std::make_optional(FakeElement(registry_, tree.firstChild()))
               : std::nullopt;
  }

  std::optional<FakeElement> lastChild() const {
    auto& tree = registry_.get().get<svg::components::TreeComponent>(entity_);
    return tree.lastChild() != entt::null
               ? std::make_optional(FakeElement(registry_, tree.lastChild()))
               : std::nullopt;
  }

  std::optional<FakeElement> previousSibling() const {
    auto& tree = registry_.get().get<svg::components::TreeComponent>(entity_);
    return tree.previousSibling() != entt::null
               ? std::make_optional(FakeElement(registry_, tree.previousSibling()))
               : std::nullopt;
  }

  std::optional<FakeElement> nextSibling() const {
    auto& tree = registry_.get().get<svg::components::TreeComponent>(entity_);
    return tree.nextSibling() != entt::null
               ? std::make_optional(FakeElement(registry_, tree.nextSibling()))
               : std::nullopt;
  }

private:
  std::reference_wrapper<svg::Registry> registry_;
  svg::Entity entity_;
};

class SelectorTests : public testing::Test {
protected:
  svg::Entity createEntity(svg::XMLQualifiedNameRef xmlTypeName) {
    auto entity = registry_.create();
    registry_.emplace<svg::components::TreeComponent>(entity, svg::ElementType::Unknown,
                                                      svg::XMLQualifiedNameRef(xmlTypeName));
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

  bool doesNotMatch(std::string_view selector, FakeElement element) {
    auto maybeSelector = SelectorParser::Parse(selector);
    EXPECT_THAT(maybeSelector, NoParseError());
    if (maybeSelector.hasError()) {
      return false;
    }

    return !maybeSelector.result().matches(element).matched;
  }

  void setId(svg::Entity entity, std::string_view id) {
    registry_.get_or_emplace<FakeElementData>(entity).id = id;
  }

  void setClassName(svg::Entity entity, std::string_view className) {
    registry_.get_or_emplace<FakeElementData>(entity).className = className;
  }

  void setAttribute(svg::Entity entity, const svg::XMLQualifiedName& name, const RcString& value) {
    registry_.get_or_emplace<svg::components::AttributesComponent>(entity).setAttribute(name,
                                                                                        value);
  }

  FakeElement element(svg::Entity entity) { return FakeElement(registry_, entity); }
  svg::components::TreeComponent& tree(svg::Entity entity) {
    return registry_.get<svg::components::TreeComponent>(entity);
  }

  svg::Registry registry_;
};

TEST_F(SelectorTests, TypeMatch) {
  auto root = createEntity("rect");
  auto child1 = createEntity("a");
  auto child2 = createEntity("elm");
  auto child3 = createEntity(svg::XMLQualifiedNameRef("my-namespace", "elm"));

  tree(root).appendChild(registry_, child1);

  EXPECT_TRUE(matches("rect", element(root)));
  EXPECT_TRUE(matches("a", element(child1)));
  EXPECT_FALSE(matches("rect", element(child1)));

  EXPECT_TRUE(matches("*", element(root)));
  EXPECT_TRUE(matches("*", element(child1)));

  // Namespace matching.
  EXPECT_TRUE(matches("|a", element(child1)));
  EXPECT_FALSE(matches("|a", element(child2)));

  EXPECT_TRUE(matches("|elm", element(child2)));
  EXPECT_FALSE(matches("my-namespace|elm", element(child2)));

  EXPECT_FALSE(matches("|elm", element(child3)));
  EXPECT_TRUE(matches("my-namespace|elm", element(child3)));

  // Wildcards match both.
  EXPECT_TRUE(matches("*|elm", element(child2)));
  EXPECT_TRUE(matches("*|elm", element(child3)));
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

TEST_F(SelectorTests, AttributeMatch) {
  auto root = createEntity("rect");
  auto child1 = createEntity("a");

  tree(root).appendChild(registry_, child1);
  setAttribute(root, svg::XMLQualifiedName("attr"), "value");
  setAttribute(child1, svg::XMLQualifiedName("my-namespace", "attr"), "value2");
  setAttribute(root, svg::XMLQualifiedName("list"), "abc def a");
  setAttribute(child1, svg::XMLQualifiedName("list"), "ABC DEF A");
  setAttribute(root, svg::XMLQualifiedName("dash"), "one-two-three");
  setAttribute(child1, svg::XMLQualifiedName("dash"), "ONE-two-THree");
  setAttribute(root, svg::XMLQualifiedName("long"), "the quick brown fox");
  setAttribute(child1, svg::XMLQualifiedName("long"), "THE QUICK BROWN FOX");

  // Use the same attribute name with different namespaces on root.
  setAttribute(root, svg::XMLQualifiedName("dupe"), "value1");
  setAttribute(root, svg::XMLQualifiedName("my-namespace", "dupe"), "value2");

  // No matcher: Matches if the attribute exists.
  EXPECT_TRUE(matches("[attr]", element(root)));
  EXPECT_FALSE(matches("[attr]", element(child1)));
  EXPECT_FALSE(matches("[doesNotExist]", element(root)));

  // Attribute namespaces
  EXPECT_TRUE(matches("[*|attr]", element(root)));
  EXPECT_TRUE(matches("[*|attr]", element(child1)));
  EXPECT_TRUE(doesNotMatch("[*|none]", element(child1)));

  EXPECT_TRUE(matches("[|attr]", element(root)));
  EXPECT_TRUE(doesNotMatch("[|attr]", element(child1)));
  EXPECT_TRUE(doesNotMatch("[my-namespace|attr]", element(root)));
  EXPECT_TRUE(matches("[my-namespace|attr]", element(child1)));

  EXPECT_TRUE(doesNotMatch("[*|attr=invalid]", element(root)));
  EXPECT_TRUE(doesNotMatch("[*|attr=invalid]", element(child1)));

  // Attribute namespaces will match values from both attributes
  EXPECT_TRUE(matches("[*|attr ^= value]", element(root)));
  EXPECT_TRUE(matches("[*|attr ^= value]", element(child1)));
  EXPECT_TRUE(matches("[*|dupe = value1]", element(root)));
  EXPECT_TRUE(matches("[*|dupe = value2]", element(root)));

  // Includes [attr ~= str]: Matches if the attribute is a space-separated list of strings and one
  // of them exactly matches.
  EXPECT_TRUE(matches("[list~=abc]", element(root)));
  EXPECT_TRUE(matches("[list~=\"abc\"]", element(root)));
  EXPECT_FALSE(matches("[list~=ABC]", element(root)));
  EXPECT_TRUE(matches("[list~=def]", element(root)));
  EXPECT_TRUE(matches("[list~=a]", element(root)));
  EXPECT_FALSE(matches("[list~=b]", element(root)));

  // Includes [attr ~= str i] (case-insensitive).
  EXPECT_TRUE(matches("[list~=abc i]", element(root)));
  EXPECT_TRUE(matches("[list~=\"abc\" i]", element(root)));
  EXPECT_TRUE(matches("[list~=abc i]", element(child1)));
  EXPECT_TRUE(matches("[list~=ABC i]", element(root)));
  EXPECT_TRUE(matches("[list~=ABC i]", element(child1)));

  // DashMatch [attr |= str]: Matches if the attribute exactly matches or matches the start of the
  // value plus a hyphen.
  EXPECT_TRUE(matches("[dash|=one]", element(root)));
  EXPECT_TRUE(matches("[dash|=one-two]", element(root)));
  EXPECT_TRUE(matches("[dash|=one-two-three]", element(root)));
  EXPECT_TRUE(matches("[dash|=\"one-two-three\"]", element(root)));
  EXPECT_FALSE(matches("[dash|=one-]", element(root)));
  EXPECT_FALSE(matches("[dash|=invalid]", element(root)));

  // DashMatch [attr |= str i] (case-insensitive).
  EXPECT_TRUE(matches("[dash|=one i]", element(root)));
  EXPECT_TRUE(matches("[dash|=ONE i]", element(root)));
  EXPECT_TRUE(matches("[dash|=\"ONE\" i]", element(root)));
  EXPECT_TRUE(matches("[dash|=one i]", element(child1)));
  EXPECT_TRUE(matches("[dash|=ONE i]", element(child1)));
  EXPECT_TRUE(matches("[dash|=\"ONE\" i]", element(child1)));

  EXPECT_TRUE(matches("[dash|=one-two-three i]", element(root)));
  EXPECT_TRUE(matches("[dash|=one-two-three i]", element(child1)));
  EXPECT_FALSE(matches("[dash|=INVALID i]", element(root)));

  // PrefixMatch [attr ^= str]: Matches if the attribute starts with the given string.
  EXPECT_TRUE(matches("[long^=the]", element(root)));
  EXPECT_TRUE(matches("[long^=\"the \"]", element(root)));
  EXPECT_TRUE(matches("[long$=\"the quick brown fox\"]", element(root)));
  EXPECT_TRUE(matches("[long^=\"the qui\"]", element(root)));
  EXPECT_FALSE(matches("[long^=\"the long\"]", element(root)));

  // PrefixMatch [attr ^= str i] (case-insensitive).
  EXPECT_TRUE(matches("[long^=THE i]", element(root)));
  EXPECT_TRUE(matches("[long^=the i]", element(child1)));
  EXPECT_TRUE(matches("[long^=\"THE \" i]", element(root)));
  EXPECT_TRUE(matches("[long^=\"the \" i]", element(child1)));
  EXPECT_TRUE(matches("[long^=\"the qui\" i]", element(child1)));
  EXPECT_FALSE(matches("[long^=\"the long\" i]", element(child1)));

  // SuffixMatch [attr $= str]: Matches if the attribute ends with the given string.
  EXPECT_TRUE(matches("[long$=fox]", element(root)));
  EXPECT_TRUE(matches("[long$=\" fox\"]", element(root)));
  EXPECT_TRUE(matches("[long$=\"brown fox\"]", element(root)));
  EXPECT_TRUE(matches("[long$=\"the quick brown fox\"]", element(root)));
  EXPECT_FALSE(matches("[long$=\"foxes\"]", element(root)));

  // SuffixMatch [attr $= str i] (case-insensitive).
  EXPECT_TRUE(matches("[long$=FOX i]", element(root)));
  EXPECT_TRUE(matches("[long$=fox i]", element(child1)));
  EXPECT_TRUE(matches("[long$=\" FOX\" i]", element(root)));
  EXPECT_TRUE(matches("[long$=\" fox\" i]", element(child1)));
  EXPECT_TRUE(matches("[long$=\"brown fox\" i]", element(child1)));
  EXPECT_TRUE(matches("[long$=\"the quick brown fox\" i]", element(child1)));
  EXPECT_FALSE(matches("[long$=\"foxes\" i]", element(child1)));

  // SubstringMatch [attr *= str]: Matches if the attribute contains the given string.
  EXPECT_TRUE(matches("[long*=brown]", element(root)));
  EXPECT_TRUE(matches("[long*=\"brown\"]", element(root)));
  EXPECT_TRUE(matches("[long*=\"quick brown fox\"]", element(root)));
  EXPECT_TRUE(matches("[long*=\"the quick brown fox\"]", element(root)));
  EXPECT_FALSE(matches("[long*=\"the quick brown foxes\"]", element(root)));

  // SubstringMatch [attr *= str i] (case-insensitive).
  EXPECT_TRUE(matches("[long*=BROWN i]", element(root)));
  EXPECT_TRUE(matches("[long*=brown i]", element(child1)));
  EXPECT_TRUE(matches("[long*=\"FOX\" i]", element(root)));
  EXPECT_TRUE(matches("[long*=\"fox\" i]", element(child1)));
  EXPECT_TRUE(matches("[long*=\"quick brown fox\" i]", element(child1)));
  EXPECT_TRUE(matches("[long*=\"the quick brown fox\" i]", element(child1)));
  EXPECT_FALSE(matches("[long*=\"the quick brown foxes\" i]", element(child1)));

  // Eq [attr = str]: Matches if the attribute exactly matches the given string.
  EXPECT_TRUE(matches("[attr=value]", element(root)));
  EXPECT_FALSE(matches("[attr=invalid]", element(root)));
  EXPECT_TRUE(matches("[list=\"abc def a\"]", element(root)));
  EXPECT_FALSE(matches("[list=\"abc def a\"]", element(child1)));
  EXPECT_TRUE(matches("[list=\"ABC DEF A\"]", element(child1)));
  EXPECT_TRUE(matches("[dash=one-two-three]", element(root)));
  EXPECT_TRUE(matches("[dash=ONE-two-THree]", element(child1)));
  EXPECT_FALSE(matches("[dash=INVALID]", element(root)));
  EXPECT_TRUE(matches("[long=\"the quick brown fox\"]", element(root)));
  EXPECT_FALSE(matches("[long=\"the quick brown\"]", element(root)));

  // Eq [attr = str i] (case-insensitive).
  EXPECT_TRUE(matches("[attr=VALUE i]", element(root)));
  EXPECT_FALSE(matches("[attr=INVALID i]", element(root)));
  EXPECT_TRUE(matches("[list=\"ABC DEF A\" i]", element(root)));
  EXPECT_TRUE(matches("[list=\"abc def a\" i]", element(child1)));
  EXPECT_TRUE(matches("[dash=one-two-three i]", element(root)));
  EXPECT_TRUE(matches("[dash=one-two-three i]", element(child1)));
  EXPECT_FALSE(matches("[dash=INVALID i]", element(root)));
  EXPECT_TRUE(matches("[long=\"THE QUICK BROWN FOX\" i]", element(root)));
  EXPECT_FALSE(matches("[long=\"THE QUICK BROWN\" i]", element(root)));
}

TEST_F(SelectorTests, PseudoClassSelectorSimple) {
  // <root>
  // -> midA = <mid>
  //   -> childA = <a>
  //   -> childB = <b>
  //   -> childC = <c>
  // -> midB = <mid>
  //  -> childD = <d>
  auto root = createEntity("root");
  auto midA = createEntity("mid");
  auto midB = createEntity("mid");
  auto childA = createEntity("a");
  auto childB = createEntity("b");
  auto childC = createEntity("c");
  auto childD = createEntity("d");

  tree(root).appendChild(registry_, midA);
  tree(root).appendChild(registry_, midB);
  tree(midA).appendChild(registry_, childA);
  tree(midA).appendChild(registry_, childB);
  tree(midA).appendChild(registry_, childC);
  tree(midB).appendChild(registry_, childD);

  // :root
  EXPECT_TRUE(matches(":root", element(root)));
  EXPECT_TRUE(doesNotMatch(":root", element(midA)));
  EXPECT_TRUE(matches(":root > mid", element(midA)));
  EXPECT_TRUE(matches(":root > mid", element(midB)));
  EXPECT_TRUE(doesNotMatch(":root > a", element(childA)));

  // :empty
  EXPECT_TRUE(doesNotMatch(":empty", element(root)));
  EXPECT_TRUE(matches(":empty", element(childA)));

  // :first-child
  EXPECT_TRUE(matches(":first-child", element(root)));
  EXPECT_TRUE(matches(":first-child", element(midA)));
  EXPECT_TRUE(doesNotMatch(":first-child", element(midB)));
  EXPECT_TRUE(matches(":first-child", element(childA)));

  // :last-child
  EXPECT_TRUE(matches(":last-child", element(root)));
  EXPECT_TRUE(doesNotMatch(":last-child", element(midA)));
  EXPECT_TRUE(matches(":last-child", element(midB)));
  EXPECT_TRUE(doesNotMatch(":last-child", element(childA)));
  EXPECT_TRUE(matches(":last-child", element(childC)));
  EXPECT_TRUE(matches(":last-child", element(childD)));

  // :only-child
  EXPECT_TRUE(matches(":only-child", element(root)));
  EXPECT_TRUE(doesNotMatch(":only-child", element(midA)));
  EXPECT_TRUE(doesNotMatch(":only-child", element(midB)));
  EXPECT_TRUE(doesNotMatch(":only-child", element(childA)));
  EXPECT_TRUE(matches(":only-child", element(childD)));
}

TEST_F(SelectorTests, PseudoClassSelectorNthChild) {
  // <root>
  // -> mid1 = <mid>
  //   -> child1 = <type1>
  //   -> child2 = <type2> (alternating 1/2 based on if number if even)
  //      ...
  //   -> child8 = <type2>
  auto root = createEntity("root");
  auto mid1 = createEntity("mid");
  std::map<std::string, svg::Entity> children;

  tree(root).appendChild(registry_, mid1);
  for (int i = 1; i <= 8; ++i) {
    const std::string id = "child" + std::to_string(i);
    const std::string typeName = "type" + std::to_string((i - 1) % 2 + 1);
    children[id] = createEntity(svg::XMLQualifiedNameRef(typeName));
    tree(mid1).appendChild(registry_, children[id]);
  }

  // :nth-child(An+B) without a selector
  EXPECT_TRUE(matches(":nth-child(1)", element(children["child1"])));
  EXPECT_TRUE(doesNotMatch(":nth-child(1)", element(root))) << "Should not match root element";

  EXPECT_TRUE(doesNotMatch(":nth-child(2n)", element(children["child1"])));
  EXPECT_TRUE(matches(":nth-child(2n)", element(children["child2"])));
  EXPECT_TRUE(doesNotMatch(":nth-child(2n)", element(children["child3"])));

  // :nth-child(An+B of S) with a selector
  EXPECT_TRUE(matches(":nth-child(1 of type1)", element(children["child1"])));
  EXPECT_TRUE(doesNotMatch(":nth-child(1 of type2)", element(children["child1"])));

  EXPECT_TRUE(doesNotMatch(":nth-child(2n of type1)", element(children["child1"])));
  EXPECT_TRUE(doesNotMatch(":nth-child(2n of type1)", element(children["child2"])));
  EXPECT_TRUE(matches(":nth-child(2n of type1)", element(children["child3"])));
  EXPECT_TRUE(doesNotMatch(":nth-child(2n of type1)", element(children["child5"])));

  // :nth-last-child(...)
  EXPECT_TRUE(doesNotMatch(":nth-last-child(1)", element(children["child1"])));
  EXPECT_TRUE(matches(":nth-last-child(1)", element(children["child8"])));
  EXPECT_TRUE(doesNotMatch(":nth-last-child(1)", element(root))) << "Should not match root element";

  EXPECT_TRUE(matches(":nth-last-child(2n)", element(children["child1"])));       // 8
  EXPECT_TRUE(doesNotMatch(":nth-last-child(2n)", element(children["child2"])));  // 7
  EXPECT_TRUE(matches(":nth-last-child(2n)", element(children["child7"])));       // 2
  EXPECT_TRUE(doesNotMatch(":nth-last-child(2n)", element(children["child8"])));  // 1

  // :nth-of-type(...)
  EXPECT_TRUE(matches(":nth-of-type(1)", element(children["child1"])));
  EXPECT_TRUE(matches(":nth-of-type(1)", element(children["child2"])));
  EXPECT_TRUE(doesNotMatch(":nth-of-type(1)", element(children["child3"])));
  EXPECT_TRUE(doesNotMatch(":nth-of-type(1)", element(children["child4"])));

  EXPECT_TRUE(doesNotMatch(":nth-of-type(2)", element(children["child1"])));
  EXPECT_TRUE(doesNotMatch(":nth-of-type(2)", element(children["child2"])));
  EXPECT_TRUE(matches(":nth-of-type(2)", element(children["child3"])));
  EXPECT_TRUE(matches(":nth-of-type(2)", element(children["child4"])));

  // [of S] is not supported
  EXPECT_TRUE(doesNotMatch(":nth-of-type(1 of type1)", element(children["child1"])));

  // :nth-last-of-type(...)
  EXPECT_TRUE(doesNotMatch(":nth-last-of-type(1)", element(children["child1"])));
  EXPECT_TRUE(doesNotMatch(":nth-last-of-type(1)", element(children["child2"])));
  EXPECT_TRUE(matches(":nth-last-of-type(1)", element(children["child8"])));
  EXPECT_TRUE(matches(":nth-last-of-type(1)", element(children["child7"])));
  EXPECT_TRUE(doesNotMatch(":nth-last-of-type(1)", element(children["child6"])));
  EXPECT_TRUE(doesNotMatch(":nth-last-of-type(1)", element(children["child5"])));

  // [of S] is not supported
  EXPECT_TRUE(doesNotMatch(":nth-last-of-type(1 of type2)", element(children["child8"])));

  // :first-of-type
  EXPECT_TRUE(matches(":first-of-type", element(children["child1"])));
  EXPECT_TRUE(matches(":first-of-type", element(children["child2"])));
  EXPECT_TRUE(doesNotMatch(":first-of-type", element(children["child3"])));
  EXPECT_TRUE(doesNotMatch(":first-of-type", element(children["child4"])));

  // :last-of-type
  EXPECT_TRUE(doesNotMatch(":last-of-type", element(children["child1"])));
  EXPECT_TRUE(doesNotMatch(":last-of-type", element(children["child2"])));
  EXPECT_TRUE(matches(":last-of-type", element(children["child8"])));
  EXPECT_TRUE(matches(":last-of-type", element(children["child7"])));

  // :only-of-type
  EXPECT_TRUE(doesNotMatch(":only-of-type", element(children["child1"])));
  EXPECT_TRUE(doesNotMatch(":only-of-type", element(children["child2"])));
  EXPECT_TRUE(matches(":only-of-type", element(mid1)));
}

}  // namespace donner::css::parser