#include "donner/css/Selector.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>

#include "donner/base/RcString.h"
#include "donner/base/element/tests/FakeElement.h"
#include "donner/base/parser/tests/ParseResultTestUtils.h"
#include "donner/css/Specificity.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/css/tests/SelectorTestUtils.h"

using testing::ElementsAre;
using testing::ElementsAreArray;

namespace donner::css::parser {

using namespace base::parser;  // NOLINT: For tests

class SelectorTests : public testing::Test {
protected:
  bool matches(std::string_view selector, const FakeElement& element) {
    auto maybeSelector = SelectorParser::Parse(selector);
    EXPECT_THAT(maybeSelector, NoParseError());
    if (maybeSelector.hasError()) {
      return false;
    }

    return maybeSelector.result().matches(element).matched;
  }

  bool doesNotMatch(std::string_view selector, const FakeElement& element) {
    auto maybeSelector = SelectorParser::Parse(selector);
    EXPECT_THAT(maybeSelector, NoParseError());
    if (maybeSelector.hasError()) {
      return false;
    }

    return !maybeSelector.result().matches(element).matched;
  }
};

TEST_F(SelectorTests, TypeMatch) {
  FakeElement root("rect");
  FakeElement child1("a");
  FakeElement child2("elm");
  FakeElement child3(XMLQualifiedNameRef("my-namespace", "elm"));

  root.appendChild(child1);

  SCOPED_TRACE(testing::Message() << "*** Tree structure:\n" << root.printAsTree() << "\n");

  EXPECT_TRUE(matches("rect", root));
  EXPECT_TRUE(matches("a", child1));
  EXPECT_FALSE(matches("rect", child1));

  EXPECT_TRUE(matches("*", root));
  EXPECT_TRUE(matches("*", child1));

  // Namespace matching.
  EXPECT_TRUE(matches("|a", child1));
  EXPECT_FALSE(matches("|a", child2));

  EXPECT_TRUE(matches("|elm", child2));
  EXPECT_FALSE(matches("my-namespace|elm", child2));

  EXPECT_FALSE(matches("|elm", child3));
  EXPECT_TRUE(matches("my-namespace|elm", child3));

  // Wildcards match both.
  EXPECT_TRUE(matches("*|elm", child2));
  EXPECT_TRUE(matches("*|elm", child3));
}

TEST_F(SelectorTests, Combinators) {
  FakeElement root("root");
  FakeElement mid("mid");
  FakeElement childA("a");
  FakeElement childB("b");
  FakeElement childC("c");
  FakeElement childD("d");

  root.appendChild(mid);
  mid.appendChild(childA);
  mid.appendChild(childB);
  mid.appendChild(childC);
  mid.appendChild(childD);

  SCOPED_TRACE(testing::Message() << "*** Tree structure:\n" << root.printAsTree() << "\n");

  EXPECT_TRUE(matches("root a", childA));
  EXPECT_FALSE(matches("root > a", childA));
  EXPECT_TRUE(matches("root > mid", mid));
  EXPECT_TRUE(matches("a + b", childB));
  EXPECT_FALSE(matches("a + c", childC));
  EXPECT_TRUE(matches("a ~ c", childC));
  EXPECT_TRUE(matches("b ~ c", childC));
  EXPECT_TRUE(matches("root > mid a + b ~ d", childD));
  EXPECT_FALSE(matches("root > mid a + b ~ d", childC));
}

TEST_F(SelectorTests, AttributeMatch) {
  FakeElement root("rect");
  FakeElement child1("a");

  root.appendChild(child1);
  root.setAttribute(XMLQualifiedName("attr"), "value");
  child1.setAttribute(XMLQualifiedName("my-namespace", "attr"), "value2");
  root.setAttribute(XMLQualifiedName("list"), "abc def a");
  child1.setAttribute(XMLQualifiedName("list"), "ABC DEF A");
  root.setAttribute(XMLQualifiedName("dash"), "one-two-three");
  child1.setAttribute(XMLQualifiedName("dash"), "ONE-two-THree");
  root.setAttribute(XMLQualifiedName("long"), "the quick brown fox");
  child1.setAttribute(XMLQualifiedName("long"), "THE QUICK BROWN FOX");

  // Use the same attribute name with different namespaces on root.
  root.setAttribute(XMLQualifiedName("dupe"), "value1");
  root.setAttribute(XMLQualifiedName("my-namespace", "dupe"), "value2");

  SCOPED_TRACE(testing::Message() << "*** Tree structure:\n" << root.printAsTree() << "\n");

  // No matcher: Matches if the attribute exists.
  EXPECT_TRUE(matches("[attr]", root));
  EXPECT_FALSE(matches("[attr]", child1));
  EXPECT_FALSE(matches("[doesNotExist]", root));

  // Attribute namespaces
  EXPECT_TRUE(matches("[*|attr]", root));
  EXPECT_TRUE(matches("[*|attr]", child1));
  EXPECT_TRUE(doesNotMatch("[*|none]", child1));

  EXPECT_TRUE(matches("[|attr]", root));
  EXPECT_TRUE(doesNotMatch("[|attr]", child1));
  EXPECT_TRUE(doesNotMatch("[my-namespace|attr]", root));
  EXPECT_TRUE(matches("[my-namespace|attr]", child1));

  EXPECT_TRUE(doesNotMatch("[*|attr=invalid]", root));
  EXPECT_TRUE(doesNotMatch("[*|attr=invalid]", child1));

  // Attribute namespaces will match values from both attributes
  EXPECT_TRUE(matches("[*|attr ^= value]", root));
  EXPECT_TRUE(matches("[*|attr ^= value]", child1));
  EXPECT_TRUE(matches("[*|dupe = value1]", root));
  EXPECT_TRUE(matches("[*|dupe = value2]", root));

  // Includes [attr ~= str]: Matches if the attribute is a space-separated list of strings and one
  // of them exactly matches.
  EXPECT_TRUE(matches("[list~=abc]", root));
  EXPECT_TRUE(matches("[list~=\"abc\"]", root));
  EXPECT_FALSE(matches("[list~=ABC]", root));
  EXPECT_TRUE(matches("[list~=def]", root));
  EXPECT_TRUE(matches("[list~=a]", root));
  EXPECT_FALSE(matches("[list~=b]", root));

  // Includes [attr ~= str i] (case-insensitive).
  EXPECT_TRUE(matches("[list~=abc i]", root));
  EXPECT_TRUE(matches("[list~=\"abc\" i]", root));
  EXPECT_TRUE(matches("[list~=abc i]", child1));
  EXPECT_TRUE(matches("[list~=ABC i]", root));
  EXPECT_TRUE(matches("[list~=ABC i]", child1));

  // DashMatch [attr |= str]: Matches if the attribute exactly matches or matches the start of the
  // value plus a hyphen.
  EXPECT_TRUE(matches("[dash|=one]", root));
  EXPECT_TRUE(matches("[dash|=one-two]", root));
  EXPECT_TRUE(matches("[dash|=one-two-three]", root));
  EXPECT_TRUE(matches("[dash|=\"one-two-three\"]", root));
  EXPECT_FALSE(matches("[dash|=one-]", root));
  EXPECT_FALSE(matches("[dash|=invalid]", root));

  // DashMatch [attr |= str i] (case-insensitive).
  EXPECT_TRUE(matches("[dash|=one i]", root));
  EXPECT_TRUE(matches("[dash|=ONE i]", root));
  EXPECT_TRUE(matches("[dash|=\"ONE\" i]", root));
  EXPECT_TRUE(matches("[dash|=one i]", child1));
  EXPECT_TRUE(matches("[dash|=ONE i]", child1));
  EXPECT_TRUE(matches("[dash|=\"ONE\" i]", child1));

  EXPECT_TRUE(matches("[dash|=one-two-three i]", root));
  EXPECT_TRUE(matches("[dash|=one-two-three i]", child1));
  EXPECT_FALSE(matches("[dash|=INVALID i]", root));

  // PrefixMatch [attr ^= str]: Matches if the attribute starts with the given string.
  EXPECT_TRUE(matches("[long^=the]", root));
  EXPECT_TRUE(matches("[long^=\"the \"]", root));
  EXPECT_TRUE(matches("[long$=\"the quick brown fox\"]", root));
  EXPECT_TRUE(matches("[long^=\"the qui\"]", root));
  EXPECT_FALSE(matches("[long^=\"the long\"]", root));

  // PrefixMatch [attr ^= str i] (case-insensitive).
  EXPECT_TRUE(matches("[long^=THE i]", root));
  EXPECT_TRUE(matches("[long^=the i]", child1));
  EXPECT_TRUE(matches("[long^=\"THE \" i]", root));
  EXPECT_TRUE(matches("[long^=\"the \" i]", child1));
  EXPECT_TRUE(matches("[long^=\"the qui\" i]", child1));
  EXPECT_FALSE(matches("[long^=\"the long\" i]", child1));

  // SuffixMatch [attr $= str]: Matches if the attribute ends with the given string.
  EXPECT_TRUE(matches("[long$=fox]", root));
  EXPECT_TRUE(matches("[long$=\" fox\"]", root));
  EXPECT_TRUE(matches("[long$=\"brown fox\"]", root));
  EXPECT_TRUE(matches("[long$=\"the quick brown fox\"]", root));
  EXPECT_FALSE(matches("[long$=\"foxes\"]", root));

  // SuffixMatch [attr $= str i] (case-insensitive).
  EXPECT_TRUE(matches("[long$=FOX i]", root));
  EXPECT_TRUE(matches("[long$=fox i]", child1));
  EXPECT_TRUE(matches("[long$=\" FOX\" i]", root));
  EXPECT_TRUE(matches("[long$=\" fox\" i]", child1));
  EXPECT_TRUE(matches("[long$=\"brown fox\" i]", child1));
  EXPECT_TRUE(matches("[long$=\"the quick brown fox\" i]", child1));
  EXPECT_FALSE(matches("[long$=\"foxes\" i]", child1));

  // SubstringMatch [attr *= str]: Matches if the attribute contains the given string.
  EXPECT_TRUE(matches("[long*=brown]", root));
  EXPECT_TRUE(matches("[long*=\"brown\"]", root));
  EXPECT_TRUE(matches("[long*=\"quick brown fox\"]", root));
  EXPECT_TRUE(matches("[long*=\"the quick brown fox\"]", root));
  EXPECT_FALSE(matches("[long*=\"the quick brown foxes\"]", root));

  // SubstringMatch [attr *= str i] (case-insensitive).
  EXPECT_TRUE(matches("[long*=BROWN i]", root));
  EXPECT_TRUE(matches("[long*=brown i]", child1));
  EXPECT_TRUE(matches("[long*=\"FOX\" i]", root));
  EXPECT_TRUE(matches("[long*=\"fox\" i]", child1));
  EXPECT_TRUE(matches("[long*=\"quick brown fox\" i]", child1));
  EXPECT_TRUE(matches("[long*=\"the quick brown fox\" i]", child1));
  EXPECT_FALSE(matches("[long*=\"the quick brown foxes\" i]", child1));

  // Eq [attr = str]: Matches if the attribute exactly matches the given string.
  EXPECT_TRUE(matches("[attr=value]", root));
  EXPECT_FALSE(matches("[attr=invalid]", root));
  EXPECT_TRUE(matches("[list=\"abc def a\"]", root));
  EXPECT_FALSE(matches("[list=\"abc def a\"]", child1));
  EXPECT_TRUE(matches("[list=\"ABC DEF A\"]", child1));
  EXPECT_TRUE(matches("[dash=one-two-three]", root));
  EXPECT_TRUE(matches("[dash=ONE-two-THree]", child1));
  EXPECT_FALSE(matches("[dash=INVALID]", root));
  EXPECT_TRUE(matches("[long=\"the quick brown fox\"]", root));
  EXPECT_FALSE(matches("[long=\"the quick brown\"]", root));

  // Eq [attr = str i] (case-insensitive).
  EXPECT_TRUE(matches("[attr=VALUE i]", root));
  EXPECT_FALSE(matches("[attr=INVALID i]", root));
  EXPECT_TRUE(matches("[list=\"ABC DEF A\" i]", root));
  EXPECT_TRUE(matches("[list=\"abc def a\" i]", child1));
  EXPECT_TRUE(matches("[dash=one-two-three i]", root));
  EXPECT_TRUE(matches("[dash=one-two-three i]", child1));
  EXPECT_FALSE(matches("[dash=INVALID i]", root));
  EXPECT_TRUE(matches("[long=\"THE QUICK BROWN FOX\" i]", root));
  EXPECT_FALSE(matches("[long=\"THE QUICK BROWN\" i]", root));
}

TEST_F(SelectorTests, PseudoClassSelectorSimple) {
  // <root>
  // -> midA = <mid>
  //   -> childA = <a>
  //   -> childB = <b>
  //   -> childC = <c>
  // -> midB = <mid>
  //  -> childD = <d>
  // -> midUnknown = <unknown>
  FakeElement root("root");
  FakeElement midA("mid");
  FakeElement midB("mid");
  FakeElement midUnknown("unknown");
  FakeElement childA("a");
  FakeElement childB("b");
  FakeElement childC("c");
  FakeElement childD("d");

  root.appendChild(midA);
  root.appendChild(midB);
  root.appendChild(midUnknown);
  midA.appendChild(childA);
  midA.appendChild(childB);
  midA.appendChild(childC);
  midB.appendChild(childD);

  SCOPED_TRACE(testing::Message() << "*** Tree structure:\n" << root.printAsTree() << "\n");

  // :root
  EXPECT_TRUE(matches(":root", root));
  EXPECT_TRUE(doesNotMatch(":root", midA));
  EXPECT_TRUE(matches(":root > mid", midA));
  EXPECT_TRUE(matches(":root > mid", midB));
  EXPECT_TRUE(doesNotMatch(":root > a", childA));

  // :empty
  EXPECT_TRUE(doesNotMatch(":empty", root));
  EXPECT_TRUE(matches(":empty", childA));

  // :first-child
  EXPECT_TRUE(matches(":first-child", root));
  EXPECT_TRUE(matches(":first-child", midA));
  EXPECT_TRUE(doesNotMatch(":first-child", midB));
  EXPECT_TRUE(matches(":first-child", childA));

  // :last-child
  EXPECT_TRUE(matches(":last-child", root));
  EXPECT_TRUE(doesNotMatch(":last-child", midA));
  EXPECT_TRUE(matches(":last-child", midUnknown));
  EXPECT_TRUE(doesNotMatch(":last-child", childA));
  EXPECT_TRUE(matches(":last-child", childC));
  EXPECT_TRUE(matches(":last-child", childD));

  // :only-child
  EXPECT_TRUE(matches(":only-child", root));
  EXPECT_TRUE(doesNotMatch(":only-child", midA));
  EXPECT_TRUE(doesNotMatch(":only-child", midB));
  EXPECT_TRUE(doesNotMatch(":only-child", childA));
  EXPECT_TRUE(matches(":only-child", childD));

  // :scope
  // See https://www.w3.org/TR/2022/WD-selectors-4-20221111/#the-scope-pseudo for `:scope` rules.
  EXPECT_TRUE(doesNotMatch(":scope", root))
      << ":scope cannot match the element directly, it cannot be the subject of the selector";
  EXPECT_TRUE(doesNotMatch(":scope > root", root));
  EXPECT_TRUE(matches(":scope > mid", midA));
  EXPECT_TRUE(matches(":scope > mid", midB));
  EXPECT_TRUE(doesNotMatch(":scope > a", childA));

  // :defined
  // In the implementation for FakeElement, the "unknown" element is special and returns
  // `isKnownType() == false`. The only element with this type is midUnknown.
  EXPECT_TRUE(matches(":defined", root));
  EXPECT_TRUE(matches(":defined", midA));
  EXPECT_TRUE(matches(":defined", midB));
  EXPECT_TRUE(matches(":defined", childA));
  EXPECT_TRUE(matches(":defined", childB));
  EXPECT_TRUE(doesNotMatch(":defined", midUnknown));
}

TEST_F(SelectorTests, PseudoClassSelectorNthChild) {
  // <root>
  // -> mid1 = <mid>
  //   -> child1 = <type1>
  //   -> child2 = <type2> (alternating 1/2 based on if number if even)
  //      ...
  //   -> child8 = <type2>
  FakeElement root("root");
  FakeElement mid1("mid");
  std::map<std::string, FakeElement> children;

  root.appendChild(mid1);
  for (int i = 1; i <= 8; ++i) {
    const std::string id = "child" + std::to_string(i);
    const std::string typeName = "type" + std::to_string((i - 1) % 2 + 1);
    children[id] = FakeElement(XMLQualifiedNameRef(typeName));
    mid1.appendChild(children[id]);
  }

  SCOPED_TRACE(testing::Message() << "*** Tree structure:\n" << root.printAsTree() << "\n");

  // :nth-child(An+B) without a selector
  EXPECT_TRUE(matches(":nth-child(1)", children["child1"]));
  EXPECT_TRUE(doesNotMatch(":nth-child(1)", root)) << "Should not match root element";

  EXPECT_TRUE(doesNotMatch(":nth-child(2n)", children["child1"]));
  EXPECT_TRUE(matches(":nth-child(2n)", children["child2"]));
  EXPECT_TRUE(doesNotMatch(":nth-child(2n)", children["child3"]));

  // :nth-child(An+B of S) with a selector
  EXPECT_TRUE(matches(":nth-child(1 of type1)", children["child1"]));
  EXPECT_TRUE(doesNotMatch(":nth-child(1 of type2)", children["child1"]));

  EXPECT_TRUE(doesNotMatch(":nth-child(2n of type1)", children["child1"]));
  EXPECT_TRUE(doesNotMatch(":nth-child(2n of type1)", children["child2"]));
  EXPECT_TRUE(matches(":nth-child(2n of type1)", children["child3"]));
  EXPECT_TRUE(doesNotMatch(":nth-child(2n of type1)", children["child5"]));

  // :nth-last-child(...)
  EXPECT_TRUE(doesNotMatch(":nth-last-child(1)", children["child1"]));
  EXPECT_TRUE(matches(":nth-last-child(1)", children["child8"]));
  EXPECT_TRUE(doesNotMatch(":nth-last-child(1)", root)) << "Should not match root element";

  EXPECT_TRUE(matches(":nth-last-child(2n)", children["child1"]));       // 8
  EXPECT_TRUE(doesNotMatch(":nth-last-child(2n)", children["child2"]));  // 7
  EXPECT_TRUE(matches(":nth-last-child(2n)", children["child7"]));       // 2
  EXPECT_TRUE(doesNotMatch(":nth-last-child(2n)", children["child8"]));  // 1

  // :nth-of-type(...)
  EXPECT_TRUE(matches(":nth-of-type(1)", children["child1"]));
  EXPECT_TRUE(matches(":nth-of-type(1)", children["child2"]));
  EXPECT_TRUE(doesNotMatch(":nth-of-type(1)", children["child3"]));
  EXPECT_TRUE(doesNotMatch(":nth-of-type(1)", children["child4"]));

  EXPECT_TRUE(doesNotMatch(":nth-of-type(2)", children["child1"]));
  EXPECT_TRUE(doesNotMatch(":nth-of-type(2)", children["child2"]));
  EXPECT_TRUE(matches(":nth-of-type(2)", children["child3"]));
  EXPECT_TRUE(matches(":nth-of-type(2)", children["child4"]));

  // [of S] is not supported for :nth-of-type
  EXPECT_TRUE(doesNotMatch(":nth-of-type(1 of type1)", children["child1"]));

  // :nth-last-of-type(...)
  EXPECT_TRUE(doesNotMatch(":nth-last-of-type(1)", children["child1"]));
  EXPECT_TRUE(doesNotMatch(":nth-last-of-type(1)", children["child2"]));
  EXPECT_TRUE(matches(":nth-last-of-type(1)", children["child8"]));
  EXPECT_TRUE(matches(":nth-last-of-type(1)", children["child7"]));
  EXPECT_TRUE(doesNotMatch(":nth-last-of-type(1)", children["child6"]));
  EXPECT_TRUE(doesNotMatch(":nth-last-of-type(1)", children["child5"]));

  // [of S] is not supported
  EXPECT_TRUE(doesNotMatch(":nth-last-of-type(1 of type2)", children["child8"]));

  // :first-of-type
  EXPECT_TRUE(matches(":first-of-type", children["child1"]));
  EXPECT_TRUE(matches(":first-of-type", children["child2"]));
  EXPECT_TRUE(doesNotMatch(":first-of-type", children["child3"]));
  EXPECT_TRUE(doesNotMatch(":first-of-type", children["child4"]));

  // :last-of-type
  EXPECT_TRUE(doesNotMatch(":last-of-type", children["child1"]));
  EXPECT_TRUE(doesNotMatch(":last-of-type", children["child2"]));
  EXPECT_TRUE(matches(":last-of-type", children["child8"]));
  EXPECT_TRUE(matches(":last-of-type", children["child7"]));

  // :only-of-type
  EXPECT_TRUE(doesNotMatch(":only-of-type", children["child1"]));
  EXPECT_TRUE(doesNotMatch(":only-of-type", children["child2"]));
  EXPECT_TRUE(matches(":only-of-type", mid1));
}

TEST_F(SelectorTests, PseudoClassSelectorNthChildForgivingSelectorList) {
  // Setup: Create a simple tree structure
  FakeElement root("root");
  FakeElement parent("div");
  std::vector<FakeElement> children;

  root.appendChild(parent);
  // Create 5 children
  // - span
  // - p
  // - span
  // - p
  // - span
  for (int i = 1; i <= 5; ++i) {
    FakeElement child(i % 2 == 0 ? "p" : "span");
    children.push_back(child);
    parent.appendChild(child);
  }

  SCOPED_TRACE(testing::Message() << "*** Tree structure:\n" << root.printAsTree() << "\n");

  // Test :nth-child with forgiving selector list
  EXPECT_TRUE(matches(":nth-child(2 of p, div, span)", children[1]))
      << "Should match 2nd child, which is a p element";
  EXPECT_TRUE(matches(":nth-child(3 of span, :invalid, p)", children[2]))
      << "Should match 3rd child (span) despite invalid selector in list";
  EXPECT_FALSE(matches(":nth-child(1 of p, :invalid)", children[0]))
      << "Should not match 1st child (span) as it doesn't match any valid selector in the list";

  // Test :nth-last-child with forgiving selector list
  EXPECT_TRUE(matches(":nth-last-child(2 of p, span, :invalid)", children[3]))
      << "Should match 2nd-to-last child, which is a p element";
  EXPECT_TRUE(matches(":nth-last-child(1 of span, :invalid, div)", children[4]))
      << "Should match last child (span) despite invalid selector in list";
  EXPECT_FALSE(matches(":nth-last-child(3 of p, :invalid)", children[2]))
      << "Should not match 3rd-to-last child (span) as it doesn't match any valid selector in the "
         "list";

  // Test complex selectors within the forgiving list
  EXPECT_TRUE(matches(":nth-child(odd of span, p[class], div > *)", children[2]))
      << "Should match 3rd child (span) with complex selectors in the list";
  EXPECT_TRUE(matches(":nth-last-child(even of p, :invalid)", children[1]))
      << "Should match 2nd-to-last child (p) with complex selectors and an invalid selector";

  // Test with all invalid selectors
  EXPECT_FALSE(matches(":nth-child(1 of :invalid1, :invalid2)", children[0]))
      << "Should not match when all selectors in the list are invalid";
  EXPECT_FALSE(matches(":nth-last-child(1 of :invalid1, :invalid2)", children[4]))
      << "Should not match when all selectors in the list are invalid";
}

TEST_F(SelectorTests, PseudoClassSelectorIsNotWhereHas) {
  // <root>
  // -> mid1 = <mid>
  //   -> child1 = <type1>
  //   -> child2 = <type2> (alternating 1/2 based on if number if even)
  //      ...
  //   -> child8 = <type2>
  FakeElement root("root");
  FakeElement mid("mid");
  std::map<std::string, FakeElement> children;

  root.appendChild(mid);
  for (int i = 1; i <= 8; ++i) {
    const std::string id = "child" + std::to_string(i);
    const std::string typeName = "type" + std::to_string((i - 1) % 2 + 1);
    children[id] = FakeElement(XMLQualifiedNameRef(typeName));
    mid.appendChild(children[id]);
  }

  SCOPED_TRACE(testing::Message() << "*** Tree structure:\n" << root.printAsTree() << "\n");

  // :is(type1)
  EXPECT_TRUE(matches(":is(type1)", children["child1"]));
  EXPECT_FALSE(matches(":is(type1)", children["child2"]));

  // :not(type1)
  EXPECT_TRUE(matches(":not(type1)", children["child2"]));
  EXPECT_FALSE(matches(":not(type1)", children["child3"]));

  // :where(type1)
  EXPECT_TRUE(matches(":where(type1)", children["child1"]));
  EXPECT_FALSE(matches(":where(type1)", children["child2"]));

  // :has(> type1)
  EXPECT_TRUE(matches(":has(> type1)", mid));
  EXPECT_TRUE(doesNotMatch(":has(> type1)", root));
  EXPECT_TRUE(doesNotMatch(":has(> type1)", children["child1"]));

  // :has(type1) matches any element under the root that has a type1 child (either direct or
  // indirect)
  EXPECT_TRUE(matches(":has(type1)", root));
  EXPECT_TRUE(matches(":has(type1)", mid));
  EXPECT_TRUE(doesNotMatch(":has(type1)", children["child1"]));
}

TEST_F(SelectorTests, Specificity) {
  EXPECT_THAT(computeSpecificity("test"), SpecificityIs(Specificity::FromABC(0, 0, 1)));
  EXPECT_THAT(computeSpecificity(".test"), SpecificityIs(Specificity::FromABC(0, 1, 0)));
  EXPECT_THAT(computeSpecificity("#test"), SpecificityIs(Specificity::FromABC(1, 0, 0)));
  EXPECT_THAT(computeSpecificity("::after"), SpecificityIs(Specificity::FromABC(0, 0, 1)));
  EXPECT_THAT(computeSpecificity(":after(one)"), SpecificityIs(Specificity::FromABC(0, 1, 0)));
  EXPECT_THAT(computeSpecificity("a[attr=value]"), SpecificityIs(Specificity::FromABC(0, 1, 1)));

  EXPECT_THAT(computeSpecificity("*"), SpecificityIs(Specificity::FromABC(0, 0, 0)))
      << "Universal selectors are ignored";

  EXPECT_THAT(computeSpecificity("* > a#b.class::after"),
              SpecificityIs(Specificity::FromABC(1, 1, 2)));

  // For lists, the max specificity is computed.
  EXPECT_THAT(computeSpecificity("a, .test, #test"), SpecificityIs(Specificity::FromABC(1, 0, 0)));
  EXPECT_THAT(computeSpecificity("a, :nth-child(2)"), SpecificityIs(Specificity::FromABC(0, 1, 0)));

  // Validate pseudo-classes that change the specificity
  EXPECT_THAT(computeSpecificity(":is(a)"), SpecificityIs(Specificity::FromABC(0, 0, 1)));
  EXPECT_THAT(computeSpecificity(":not(a, #b)"), SpecificityIs(Specificity::FromABC(1, 0, 0)));
  EXPECT_THAT(computeSpecificity(":where(a)"), SpecificityIs(Specificity::FromABC(0, 0, 0)));

  // :nth-child(An+B) and :nth-of-type(An+B) have a specificity of 0,1,0, unless a selector is
  // specified, in which case the specificity of the selector is added.
  EXPECT_THAT(computeSpecificity(":nth-child(2n)"), SpecificityIs(Specificity::FromABC(0, 1, 0)));
  EXPECT_THAT(computeSpecificity(":nth-last-child(2n+1)"),
              SpecificityIs(Specificity::FromABC(0, 1, 0)));

  EXPECT_THAT(computeSpecificity(":nth-child(2n of #a)"),
              SpecificityIs(Specificity::FromABC(1, 1, 0)));
  EXPECT_THAT(computeSpecificity(":nth-last-child(2n+1 of a, [attr=value])"),
              SpecificityIs(Specificity::FromABC(0, 2, 0)));

  // S:nth-child(An+B) and :nth-child(An+B of S) have the same specificity but different behavior.
  EXPECT_THAT(computeSpecificity(":nth-child(2n+1 of S)"),
              SpecificityIs(Specificity::FromABC(0, 1, 1)));
  EXPECT_THAT(computeSpecificity("S:nth-child(2n+1)"),
              SpecificityIs(Specificity::FromABC(0, 1, 1)));

  // TODO: has() is not implemented
}

}  // namespace donner::css::parser
