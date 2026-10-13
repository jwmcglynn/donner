#include "donner/svg/SVGUnknownElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"

namespace donner::svg {
namespace {

using testing::Ne;

TEST(SVGUnknownElementTests, CreateAndCast) {
  SVGDocument document;
  SVGUnknownElement unknown = SVGUnknownElement::Create(document, "custom-element");

  EXPECT_THAT(unknown.tryCast<SVGElement>(), Ne(std::nullopt));
  EXPECT_THAT(unknown.tryCast<SVGGraphicsElement>(), Ne(std::nullopt));
  EXPECT_THAT(unknown.tryCast<SVGUnknownElement>(), Ne(std::nullopt));
  EXPECT_EQ(unknown.type(), ElementType::Unknown);
  EXPECT_EQ(unknown.tagName(), "custom-element");
  ASSERT_THAT(unknown.entityHandle().try_get<components::RenderingBehaviorComponent>(),
              testing::NotNull());
  EXPECT_THAT(unknown.entityHandle().get<components::RenderingBehaviorComponent>().behavior,
              testing::Eq(components::RenderingBehavior::Nonrenderable));
}

}  // namespace
}  // namespace donner::svg
