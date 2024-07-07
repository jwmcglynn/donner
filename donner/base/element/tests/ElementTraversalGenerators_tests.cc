#include "donner/base/element/ElementTraversalGenerators.h"

#include <gtest/gtest.h>

#include "donner/base/element/tests/FakeElement.h"
#include "donner/base/tests/BaseTestUtils.h"

namespace donner {

using testing::ElementsAre;

namespace {

template <ElementLike T>
std::vector<T> eval(ElementTraversalGenerator<T>& gen) {
  std::vector<T> result;
  while (gen.next()) {
    result.push_back(gen.getValue());
  }
  return result;
}

}  // namespace

TEST(ElementTraversalGenerators, SingleElementGenerator) {
  FakeElement root;
  ElementTraversalGenerator<FakeElement> gen = singleElementGenerator(root);

  ASSERT_TRUE(gen.next());
  const FakeElement element = gen.getValue();
  EXPECT_EQ(element, root);

  ASSERT_FALSE(gen.next());
}

TEST(ElementTraversalGenerators, ParentsGenerator) {
  FakeElement root("root");
  FakeElement child("child");
  root.appendChild(child);
  FakeElement grandchild("grandchild");
  child.appendChild(grandchild);

  ElementTraversalGenerator<FakeElement> gen = parentsGenerator(grandchild);

  EXPECT_THAT(eval(gen), ElementsAre(child, root));
}

TEST(ElementTraversalGenerators, PreviousSiblingsGenerator) {
  FakeElement root("root");
  FakeElement child1("child1");
  FakeElement child2("child2");
  FakeElement child3("child3");
  root.appendChild(child1);
  root.appendChild(child2);
  root.appendChild(child3);

  ElementTraversalGenerator<FakeElement> gen = previousSiblingsGenerator(child3);

  EXPECT_THAT(eval(gen), ElementsAre(child2, child1));
}

TEST(ElementTraversalGenerators, AllChildrenRecursiveGenerator) {
  FakeElement root("root");
  FakeElement child1("child1");
  FakeElement child2("child2");
  root.appendChild(child1);
  root.appendChild(child2);
  FakeElement grandchild1("grandchild1");
  FakeElement grandchild2("grandchild2");
  child1.appendChild(grandchild1);
  child2.appendChild(grandchild2);
  FakeElement greatGrandchild("greatGrandchild");
  grandchild1.appendChild(greatGrandchild);

  SCOPED_TRACE(testing::Message() << "*** Tree structure:\n" << root.printAsTree() << "\n");

  ElementTraversalGenerator<FakeElement> gen = allChildrenRecursiveGenerator(root);

  EXPECT_THAT(eval(gen), ElementsAre(child1, grandchild1, greatGrandchild, child2, grandchild2));
}

}  // namespace donner
