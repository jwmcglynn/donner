#include "donner/base/element/tests/FakeElement.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/xml/XMLQualifiedName.h"

namespace donner {

using testing::Eq;
using xml::XMLQualifiedNameRef;

class FakeElementTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a simple tree structure for testing
    root = FakeElement("root");
    child1 = FakeElement("child1");
    child2 = FakeElement("child2");
    grandchild = FakeElement("grandchild");

    // Set up the tree structure
    root.appendChild(child1);
    root.appendChild(child2);
    child1.appendChild(grandchild);

    // Set up some attributes and properties
    root.setId("root-id");
    root.setClassName("root-class");
    root.setAttribute(XMLQualifiedNameRef("attr1"), "value1");
    root.setAttribute(XMLQualifiedNameRef("attr2"), "value2");
  }

  void TearDown() override {
    if (testing::Test::HasFailure()) {
      std::cerr << "*** Tree structure:\n" << root.printAsTree() << "\n";
    }
  }

  FakeElement root;
  FakeElement child1;
  FakeElement child2;
  FakeElement grandchild;
};

TEST_F(FakeElementTest, Constructor) {
  FakeElement element;
  EXPECT_FALSE(element.isKnownType());
  EXPECT_EQ(element.tagName(), XMLQualifiedNameRef("unknown"));
}

TEST_F(FakeElementTest, TagNameAndType) {
  EXPECT_TRUE(root.isKnownType());
  EXPECT_EQ(root.tagName(), XMLQualifiedNameRef("root"));
}

TEST_F(FakeElementTest, IdAndClassName) {
  EXPECT_EQ(root.id(), "root-id");
  EXPECT_EQ(root.className(), "root-class");
}

TEST_F(FakeElementTest, Attributes) {
  EXPECT_EQ(root.getAttribute(XMLQualifiedNameRef("attr1")), "value1");
  EXPECT_EQ(root.getAttribute(XMLQualifiedNameRef("attr2")), "value2");
  EXPECT_FALSE(root.getAttribute(XMLQualifiedNameRef("non-existent")).has_value());
}

TEST_F(FakeElementTest, FindMatchingAttributes) {
  auto matches = root.findMatchingAttributes(XMLQualifiedNameRef("attr1"));
  EXPECT_EQ(matches.size(), 1);
  EXPECT_EQ(matches[0], XMLQualifiedNameRef("attr1"));
}

TEST_F(FakeElementTest, ParentElement) {
  EXPECT_THAT(root.parentElement(), Eq(std::nullopt));
  EXPECT_THAT(child1.parentElement(), Eq(root));
  EXPECT_THAT(grandchild.parentElement(), Eq(child1));
}

TEST_F(FakeElementTest, FirstAndLastChild) {
  EXPECT_THAT(root.firstChild(), Eq(child1));
  EXPECT_THAT(root.lastChild(), Eq(child2));
  EXPECT_THAT(grandchild.firstChild(), Eq(std::nullopt));
  EXPECT_THAT(grandchild.lastChild(), Eq(std::nullopt));
}

TEST_F(FakeElementTest, PreviousAndNextSibling) {
  EXPECT_THAT(child1.previousSibling(), Eq(std::nullopt));
  EXPECT_THAT(child1.nextSibling(), Eq(child2));
  EXPECT_THAT(child2.previousSibling(), Eq(child1));
  EXPECT_THAT(child2.nextSibling(), Eq(std::nullopt));
}

TEST_F(FakeElementTest, ElementEquality) {
  EXPECT_EQ(root, root);
  EXPECT_NE(root, child1);
}

}  // namespace donner
