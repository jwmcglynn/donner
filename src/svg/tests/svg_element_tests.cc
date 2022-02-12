#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <deque>

#include "src/svg/svg_document.h"
#include "src/svg/svg_element.h"
#include "src/svg/svg_unknown_element.h"

using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::Optional;

namespace donner::svg {

class SVGElementTests : public testing::Test {
protected:
  SVGElement create() { return SVGUnknownElement::Create(document_, "unknown"); }

  std::vector<SVGElement> children(SVGElement element) {
    std::vector<SVGElement> result;
    for (auto cur = element.firstChild(); cur; cur = cur->nextSibling()) {
      result.push_back(cur.value());
    }

    // Iterate in reverse order and verify.
    if (result.empty()) {
      EXPECT_FALSE(element.lastChild().has_value());
    } else {
      std::deque<SVGElement> resultReverse;
      for (auto cur = element.lastChild(); cur; cur = cur->previousSibling()) {
        resultReverse.push_front(cur.value());
      }

      EXPECT_THAT(resultReverse, ElementsAreArray(result));
    }

    return result;
  }

  SVGDocument document_;
};

// Basic tests for each function, extensive coverage exists in tree_component_tests.cc
TEST_F(SVGElementTests, TreeOperations) {
  auto root = create();
  auto child1 = create();
  auto child2 = create();
  auto child3 = create();

  EXPECT_EQ(child1, root.insertBefore(child1, std::nullopt));
  EXPECT_THAT(children(root), ElementsAre(child1));
  EXPECT_THAT(child1.parentElement(), Optional(root));

  EXPECT_EQ(child2, root.insertBefore(child2, child1));
  EXPECT_THAT(children(root), ElementsAre(child2, child1));

  EXPECT_EQ(child3, root.appendChild(child3));
  EXPECT_THAT(children(root), ElementsAre(child2, child1, child3));

  auto child4 = create();
  EXPECT_EQ(child4, root.replaceChild(child4, child3));
  EXPECT_THAT(children(root), ElementsAre(child2, child1, child4));

  EXPECT_EQ(child1, root.removeChild(child1));
  EXPECT_THAT(children(root), ElementsAre(child2, child4));

  child2.remove();
  EXPECT_THAT(children(root), ElementsAre(child4));

  EXPECT_EQ(root.ownerDocument(), document_);
  EXPECT_EQ(child1.ownerDocument(), document_);
  EXPECT_EQ(child2.ownerDocument(), document_);
  EXPECT_EQ(child3.ownerDocument(), document_);
  EXPECT_EQ(child4.ownerDocument(), document_);
}

TEST_F(SVGElementTests, Id) {
  auto element = create();
  EXPECT_EQ(element.id(), "");

  element.setId("test");
  EXPECT_EQ(element.id(), "test");

  element.setId("");
  EXPECT_EQ(element.id(), "");
}

}  // namespace donner::svg
