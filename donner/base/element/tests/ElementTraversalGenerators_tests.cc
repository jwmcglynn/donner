#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/element/ElementTraversalGenerators.h"

#include <gtest/gtest.h>

#include "donner/base/element/tests/FakeElement.h"

namespace donner {

TEST(ElementTraversalGenerators, SingleElementGenerator) {
  FakeElement root;
  ElementTraversalGenerator<FakeElement> gen = singleElementGenerator(root);

  ASSERT_TRUE(gen.next());
  const FakeElement element = gen.getValue();
  EXPECT_EQ(element, root);
}

}  // namespace donner
