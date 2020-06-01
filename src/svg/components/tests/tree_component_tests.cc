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
  Entity createEntity() {
    auto entity = registry_.create();
    registry_.emplace<TreeComponent>(entity, ElementType::Unknown, entity);
    return entity;
  }

  TreeComponent& tree(Entity entity) { return registry_.get<TreeComponent>(entity); }

  std::vector<Entity> children(Entity entity) {
    std::vector<Entity> result;
    for (auto cur = tree(entity).firstChild(); cur != entt::null; cur = tree(cur).nextSibling()) {
      result.push_back(cur);
    }

    // Iterate in reverse order and verify.
    if (result.empty()) {
      EXPECT_TRUE(tree(entity).lastChild() == entt::null);
    } else {
      std::deque<Entity> resultReverse;
      for (auto cur = tree(entity).lastChild(); cur != entt::null;
           cur = tree(cur).previousSibling()) {
        resultReverse.push_front(cur);
      }

      EXPECT_THAT(resultReverse, ElementsAreArray(result));
    }

    return result;
  }

  Registry registry_;
};

TEST_F(TreeComponentTests, InsertBefore) {
  auto root = createEntity();
  auto child1 = createEntity();
  auto child2 = createEntity();
  auto child3 = createEntity();
  auto child4 = createEntity();

  tree(root).insertBefore(registry_, child1, entt::null);
  EXPECT_EQ(tree(root).firstChild(), child1);
  EXPECT_EQ(tree(root).lastChild(), child1);
  EXPECT_EQ(tree(child1).parent(), root);
  EXPECT_THAT(children(root), ElementsAre(child1));

  // Inserts at beginning before only child.
  tree(root).insertBefore(registry_, child2, child1);
  EXPECT_EQ(tree(child2).parent(), root);
  EXPECT_EQ(tree(child2).nextSibling(), child1);
  EXPECT_EQ(tree(root).firstChild(), child2);
  EXPECT_EQ(tree(child1).previousSibling(), child2);
  EXPECT_THAT(children(root), ElementsAre(child2, child1));

  // Insert at end.
  tree(root).insertBefore(registry_, child3, entt::null);
  EXPECT_EQ(tree(child3).parent(), root);
  EXPECT_EQ(tree(child3).previousSibling(), child1);
  EXPECT_EQ(tree(root).lastChild(), child3);
  EXPECT_EQ(tree(child1).nextSibling(), child3);
  EXPECT_THAT(children(root), ElementsAre(child2, child1, child3));

  // Insert in middle.
  tree(root).insertBefore(registry_, child4, child1);
  EXPECT_EQ(tree(child4).parent(), root);
  EXPECT_EQ(tree(child4).previousSibling(), child2);
  EXPECT_EQ(tree(child4).nextSibling(), child1);

  EXPECT_EQ(tree(child2).nextSibling(), child4);
  EXPECT_EQ(tree(child1).previousSibling(), child4);
  EXPECT_THAT(children(root), ElementsAre(child2, child4, child1, child3));
}

TEST_F(TreeComponentTests, InsertBefore_Errors) {
  auto root = createEntity();
  EXPECT_DEATH({ tree(root).insertBefore(registry_, entt::null, entt::null); }, "newNode is null");

  auto child1 = createEntity();
  tree(root).insertBefore(registry_, child1, entt::null);

  // Wrong parent.
  auto node1 = createEntity();
  EXPECT_DEATH({ tree(root).insertBefore(registry_, child1, node1); }, "");
}

TEST_F(TreeComponentTests, InsertBefore_WithSelf) {
  auto root = createEntity();
  EXPECT_DEATH({ tree(root).insertBefore(registry_, entt::null, entt::null); }, "newNode is null");

  auto child1 = createEntity();
  tree(root).insertBefore(registry_, child1, entt::null);

  EXPECT_DEATH({ tree(root).insertBefore(registry_, child1, child1); }, "");
}

TEST_F(TreeComponentTests, InsertBefore_WithRoot) {
  auto root = createEntity();
  EXPECT_DEATH({ tree(root).insertBefore(registry_, entt::null, entt::null); }, "newNode is null");

  auto child1 = createEntity();
  EXPECT_DEATH({ tree(root).insertBefore(registry_, child1, root); }, "");
}

TEST_F(TreeComponentTests, AppendChild) {
  auto root = createEntity();
  auto child1 = createEntity();
  auto child2 = createEntity();

  tree(root).appendChild(registry_, child1);
  EXPECT_EQ(tree(root).firstChild(), child1);
  EXPECT_EQ(tree(root).lastChild(), child1);
  EXPECT_EQ(tree(child1).parent(), root);
  EXPECT_THAT(children(root), ElementsAre(child1));

  tree(root).appendChild(registry_, child2);
  EXPECT_EQ(tree(child2).parent(), root);
  EXPECT_EQ(tree(child2).previousSibling(), child1);
  EXPECT_EQ(tree(root).lastChild(), child2);
  EXPECT_EQ(tree(child1).nextSibling(), child2);

  EXPECT_THAT(children(root), ElementsAre(child1, child2));
}

TEST_F(TreeComponentTests, AppendChild_Errors) {
  auto root = createEntity();
  EXPECT_DEATH({ tree(root).appendChild(registry_, entt::null); }, "child is null");

  // Cannot insert self.
  EXPECT_DEATH({ tree(root).appendChild(registry_, root); }, "");
}

TEST_F(TreeComponentTests, ReplaceChild) {
  auto root = createEntity();
  auto child1 = createEntity();
  auto child2 = createEntity();

  // Replace with single element.
  tree(root).appendChild(registry_, child1);
  tree(root).replaceChild(registry_, child2, child1);
  EXPECT_EQ(tree(root).firstChild(), child2);
  EXPECT_EQ(tree(root).lastChild(), child2);
  EXPECT_EQ(tree(child2).parent(), root);
  EXPECT_TRUE(tree(child1).parent() == entt::null);

  auto child3 = createEntity();
  tree(root).appendChild(registry_, child1);
  tree(root).appendChild(registry_, child3);
  EXPECT_THAT(children(root), ElementsAre(child2, child1, child3));

  auto child4 = createEntity();

  // Replace first.
  tree(root).replaceChild(registry_, child4, child2);
  EXPECT_THAT(children(root), ElementsAre(child4, child1, child3));

  // Replace middle.
  tree(root).replaceChild(registry_, child2, child1);
  EXPECT_THAT(children(root), ElementsAre(child4, child2, child3));

  // Replace last.
  tree(root).replaceChild(registry_, child1, child3);
  EXPECT_THAT(children(root), ElementsAre(child4, child2, child1));
}

TEST_F(TreeComponentTests, ReplaceChild_Errors) {
  auto root = createEntity();
  auto child1 = createEntity();
  EXPECT_DEATH({ tree(root).replaceChild(registry_, entt::null, child1); }, "newChild is null");
  EXPECT_DEATH({ tree(root).replaceChild(registry_, child1, entt::null); }, "oldChild is null");

  // Cannot insert self.
  tree(root).appendChild(registry_, child1);
  EXPECT_DEATH({ tree(root).replaceChild(registry_, root, child1); }, "");

  // Wrong parent.
  auto node1 = createEntity();
  auto child2 = createEntity();
  EXPECT_DEATH({ tree(root).replaceChild(registry_, child2, node1); }, "");
}

TEST_F(TreeComponentTests, ReplaceChild_ReplaceSelf) {
  auto root = createEntity();
  auto child1 = createEntity();
  auto child2 = createEntity();
  auto child3 = createEntity();

  // Cannot insert self.
  tree(root).appendChild(registry_, child1);
  tree(root).appendChild(registry_, child2);
  tree(root).appendChild(registry_, child3);

  tree(root).replaceChild(registry_, child1, child1);
  EXPECT_THAT(children(root), ElementsAre(child1, child2, child3));

  tree(root).replaceChild(registry_, child2, child2);
  EXPECT_THAT(children(root), ElementsAre(child1, child2, child3));

  tree(root).replaceChild(registry_, child3, child3);
  EXPECT_THAT(children(root), ElementsAre(child1, child2, child3));
}

TEST_F(TreeComponentTests, RemoveChild) {
  auto root = createEntity();
  auto child1 = createEntity();

  // Remove single element.
  tree(root).appendChild(registry_, child1);
  tree(root).removeChild(registry_, child1);
  EXPECT_TRUE(tree(root).firstChild() == entt::null);
  EXPECT_TRUE(tree(root).lastChild() == entt::null);
  EXPECT_TRUE(tree(child1).parent() == entt::null);

  auto child2 = createEntity();
  auto child3 = createEntity();
  tree(root).appendChild(registry_, child1);
  tree(root).appendChild(registry_, child2);
  tree(root).appendChild(registry_, child3);
  EXPECT_THAT(children(root), ElementsAre(child1, child2, child3));

  // Remove middle.
  tree(root).removeChild(registry_, child2);
  EXPECT_THAT(children(root), ElementsAre(child1, child3));

  // Remove first.
  tree(root).removeChild(registry_, child1);
  EXPECT_THAT(children(root), ElementsAre(child3));

  tree(root).appendChild(registry_, child2);
  EXPECT_THAT(children(root), ElementsAre(child3, child2));

  // Remove last.
  tree(root).removeChild(registry_, child2);
  EXPECT_THAT(children(root), ElementsAre(child3));
}

TEST_F(TreeComponentTests, RemoveChild_Errors) {
  auto root = createEntity();
  EXPECT_DEATH({ tree(root).removeChild(registry_, entt::null); }, "child is null");

  // Cannot remove self.
  EXPECT_DEATH({ tree(root).removeChild(registry_, root); }, "");

  // Wrong parent.
  auto child1 = createEntity();
  EXPECT_DEATH({ tree(root).removeChild(registry_, child1); }, "");
}

TEST_F(TreeComponentTests, Remove) {
  auto root = createEntity();
  auto child1 = createEntity();

  // Remove single element.
  tree(root).appendChild(registry_, child1);
  tree(child1).remove(registry_);
  EXPECT_TRUE(tree(root).firstChild() == entt::null);
  EXPECT_TRUE(tree(root).lastChild() == entt::null);
  EXPECT_TRUE(tree(child1).parent() == entt::null);

  auto child2 = createEntity();
  auto child3 = createEntity();
  tree(root).appendChild(registry_, child1);
  tree(root).appendChild(registry_, child2);
  tree(root).appendChild(registry_, child3);
  EXPECT_THAT(children(root), ElementsAre(child1, child2, child3));

  // Remove middle.
  tree(child2).remove(registry_);
  EXPECT_THAT(children(root), ElementsAre(child1, child3));

  // Remove first.
  tree(child1).remove(registry_);
  EXPECT_THAT(children(root), ElementsAre(child3));

  tree(root).appendChild(registry_, child2);
  EXPECT_THAT(children(root), ElementsAre(child3, child2));

  // Remove last.
  tree(child2).remove(registry_);
  EXPECT_THAT(children(root), ElementsAre(child3));
}

}  // namespace donner
