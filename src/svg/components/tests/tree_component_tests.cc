#include <gmock/gmock.h>
#include <gtest/gtest-death-test.h>
#include <gtest/gtest.h>

#include <deque>

#include "src/svg/components/tree_component.h"

using testing::ElementsAre;
using testing::ElementsAreArray;

namespace donner {

class TreeComponentTests : public testing::Test {
protected:
  TreeComponent* createNode() {
    auto entity = registry_.create();
    registry_.emplace<TreeComponent>(entity, ElementType::Unknown, "unknown");
    return &registry_.get<TreeComponent>(entity);
  }

  std::vector<TreeComponent*> children(TreeComponent* node) {
    std::vector<TreeComponent*> result;
    for (TreeComponent* cur = node->firstChild(); cur; cur = cur->nextSibling()) {
      result.push_back(cur);
    }

    // Iterate in reverse order and verify.
    if (result.empty()) {
      EXPECT_EQ(node->lastChild(), nullptr);
    } else {
      std::deque<TreeComponent*> resultReverse;
      for (TreeComponent* cur = node->lastChild(); cur; cur = cur->previousSibling()) {
        resultReverse.push_front(cur);
      }

      EXPECT_THAT(resultReverse, ElementsAreArray(result));
    }

    return result;
  }

  Registry registry_;
};

TEST_F(TreeComponentTests, InsertBefore) {
  auto root = createNode();
  auto child1 = createNode();
  auto child2 = createNode();
  auto child3 = createNode();
  auto child4 = createNode();

  root->insertBefore(child1, nullptr);
  EXPECT_EQ(root->firstChild(), child1);
  EXPECT_EQ(root->lastChild(), child1);
  EXPECT_EQ(child1->parent(), root);
  EXPECT_THAT(children(root), ElementsAre(child1));

  // Inserts at beginning before only child.
  root->insertBefore(child2, child1);
  EXPECT_EQ(child2->parent(), root);
  EXPECT_EQ(child2->nextSibling(), child1);
  EXPECT_EQ(root->firstChild(), child2);
  EXPECT_EQ(child1->previousSibling(), child2);
  EXPECT_THAT(children(root), ElementsAre(child2, child1));

  // Insert at end.
  root->insertBefore(child3, nullptr);
  EXPECT_EQ(child3->parent(), root);
  EXPECT_EQ(child3->previousSibling(), child1);
  EXPECT_EQ(root->lastChild(), child3);
  EXPECT_EQ(child1->nextSibling(), child3);
  EXPECT_THAT(children(root), ElementsAre(child2, child1, child3));

  // Insert in middle.
  root->insertBefore(child4, child1);
  EXPECT_EQ(child4->parent(), root);
  EXPECT_EQ(child4->previousSibling(), child2);
  EXPECT_EQ(child4->nextSibling(), child1);

  EXPECT_EQ(child2->nextSibling(), child4);
  EXPECT_EQ(child1->previousSibling(), child4);
  EXPECT_THAT(children(root), ElementsAre(child2, child4, child1, child3));
}

TEST_F(TreeComponentTests, InsertBefore_Errors) {
  auto root = createNode();

  EXPECT_DEATH({ root->insertBefore(nullptr, nullptr); }, "newNode is null");

  auto child1 = createNode();
  root->insertBefore(child1, nullptr);

  // Wrong parent.
  auto node1 = createNode();
  EXPECT_DEATH({ root->insertBefore(child1, node1); }, "");
}

TEST_F(TreeComponentTests, InsertBefore_WithSelf) {
  auto root = createNode();
  EXPECT_DEATH({ root->insertBefore(nullptr, nullptr); }, "newNode is null");

  auto child1 = createNode();
  root->insertBefore(child1, nullptr);

  EXPECT_DEATH({ root->insertBefore(child1, child1); }, "");
}

TEST_F(TreeComponentTests, InsertBefore_WithRoot) {
  auto root = createNode();
  EXPECT_DEATH({ root->insertBefore(nullptr, nullptr); }, "newNode is null");

  auto child1 = createNode();
  EXPECT_DEATH({ root->insertBefore(child1, root); }, "");
}

TEST_F(TreeComponentTests, AppendChild) {
  auto root = createNode();
  auto child1 = createNode();
  auto child2 = createNode();

  root->appendChild(child1);
  EXPECT_EQ(root->firstChild(), child1);
  EXPECT_EQ(root->lastChild(), child1);
  EXPECT_EQ(child1->parent(), root);
  EXPECT_THAT(children(root), ElementsAre(child1));

  root->appendChild(child2);
  EXPECT_EQ(child2->parent(), root);
  EXPECT_EQ(child2->previousSibling(), child1);
  EXPECT_EQ(root->lastChild(), child2);
  EXPECT_EQ(child1->nextSibling(), child2);

  EXPECT_THAT(children(root), ElementsAre(child1, child2));
}

TEST_F(TreeComponentTests, AppendChild_Errors) {
  auto root = createNode();
  EXPECT_DEATH({ root->appendChild(nullptr); }, "child is null");

  // Cannot insert self.
  EXPECT_DEATH({ root->appendChild(root); }, "");
}

TEST_F(TreeComponentTests, ReplaceChild) {
  auto root = createNode();
  auto child1 = createNode();
  auto child2 = createNode();

  // Replace with single element.
  root->appendChild(child1);
  root->replaceChild(child2, child1);
  EXPECT_EQ(root->firstChild(), child2);
  EXPECT_EQ(root->lastChild(), child2);
  EXPECT_EQ(child2->parent(), root);
  EXPECT_EQ(child1->parent(), nullptr);

  auto child3 = createNode();
  root->appendChild(child1);
  root->appendChild(child3);
  EXPECT_THAT(children(root), ElementsAre(child2, child1, child3));

  auto child4 = createNode();

  // Replace first.
  root->replaceChild(child4, child2);
  EXPECT_THAT(children(root), ElementsAre(child4, child1, child3));

  // Replace middle.
  root->replaceChild(child2, child1);
  EXPECT_THAT(children(root), ElementsAre(child4, child2, child3));

  // Replace last.
  root->replaceChild(child1, child3);
  EXPECT_THAT(children(root), ElementsAre(child4, child2, child1));
}

TEST_F(TreeComponentTests, ReplaceChild_Errors) {
  auto root = createNode();
  auto child1 = createNode();
  EXPECT_DEATH({ root->replaceChild(nullptr, child1); }, "newChild is null");
  EXPECT_DEATH({ root->replaceChild(child1, nullptr); }, "oldChild is null");

  // Cannot insert self.
  root->appendChild(child1);
  EXPECT_DEATH({ root->replaceChild(root, child1); }, "");

  // Wrong parent.
  auto node1 = createNode();
  auto child2 = createNode();
  EXPECT_DEATH({ root->replaceChild(child2, node1); }, "");
}

TEST_F(TreeComponentTests, ReplaceChild_ReplaceSelf) {
  auto root = createNode();
  auto child1 = createNode();
  auto child2 = createNode();
  auto child3 = createNode();

  // Cannot insert self.
  root->appendChild(child1);
  root->appendChild(child2);
  root->appendChild(child3);

  root->replaceChild(child1, child1);
  EXPECT_THAT(children(root), ElementsAre(child1, child2, child3));

  root->replaceChild(child2, child2);
  EXPECT_THAT(children(root), ElementsAre(child1, child2, child3));

  root->replaceChild(child3, child3);
  EXPECT_THAT(children(root), ElementsAre(child1, child2, child3));
}

TEST_F(TreeComponentTests, RemoveChild) {
  auto root = createNode();
  auto child1 = createNode();

  // Remove single element.
  root->appendChild(child1);
  root->removeChild(child1);
  EXPECT_EQ(root->firstChild(), nullptr);
  EXPECT_EQ(root->lastChild(), nullptr);
  EXPECT_EQ(child1->parent(), nullptr);

  auto child2 = createNode();
  auto child3 = createNode();
  root->appendChild(child1);
  root->appendChild(child2);
  root->appendChild(child3);
  EXPECT_THAT(children(root), ElementsAre(child1, child2, child3));

  // Remove middle.
  root->removeChild(child2);
  EXPECT_THAT(children(root), ElementsAre(child1, child3));

  // Remove first.
  root->removeChild(child1);
  EXPECT_THAT(children(root), ElementsAre(child3));

  root->appendChild(child2);
  EXPECT_THAT(children(root), ElementsAre(child3, child2));

  // Remove last.
  root->removeChild(child2);
  EXPECT_THAT(children(root), ElementsAre(child3));
}

TEST_F(TreeComponentTests, RemoveChild_Errors) {
  auto root = createNode();
  EXPECT_DEATH({ root->removeChild(nullptr); }, "child is null");

  // Cannot remove self.
  EXPECT_DEATH({ root->removeChild(root); }, "");

  // Wrong parent.
  auto child1 = createNode();
  EXPECT_DEATH({ root->removeChild(child1); }, "");
}

TEST_F(TreeComponentTests, Remove) {
  auto root = createNode();
  auto child1 = createNode();

  // Remove single element.
  root->appendChild(child1);
  child1->remove();
  EXPECT_EQ(root->firstChild(), nullptr);
  EXPECT_EQ(root->lastChild(), nullptr);
  EXPECT_EQ(child1->parent(), nullptr);

  auto child2 = createNode();
  auto child3 = createNode();
  root->appendChild(child1);
  root->appendChild(child2);
  root->appendChild(child3);
  EXPECT_THAT(children(root), ElementsAre(child1, child2, child3));

  // Remove middle.
  child2->remove();
  EXPECT_THAT(children(root), ElementsAre(child1, child3));

  // Remove first.
  child1->remove();
  EXPECT_THAT(children(root), ElementsAre(child3));

  root->appendChild(child2);
  EXPECT_THAT(children(root), ElementsAre(child3, child2));

  // Remove last.
  child2->remove();
  EXPECT_THAT(children(root), ElementsAre(child3));
}

TEST_F(TreeComponentTests, Type) {
  {
    auto node = createNode();
    EXPECT_EQ(node->type(), ElementType::Unknown);
  }

  {
    auto entity = registry_.create();
    registry_.emplace<TreeComponent>(entity, ElementType::SVG, "svg");
    EXPECT_EQ(registry_.get<TreeComponent>(entity).type(), ElementType::SVG);
  }
}

TEST_F(TreeComponentTests, TypeString) {
  {
    auto node = createNode();
    EXPECT_EQ(node->typeString(), "unknown");
  }

  {
    auto entity = registry_.create();
    registry_.emplace<TreeComponent>(entity, ElementType::Unknown, "test-entity");
    EXPECT_EQ(registry_.get<TreeComponent>(entity).typeString(), "test-entity");
  }
}

}  // namespace donner
