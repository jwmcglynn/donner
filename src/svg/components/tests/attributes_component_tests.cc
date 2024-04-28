#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "gmock/gmock.h"
#include "src/base/tests/base_test_utils.h"
#include "src/svg/components/attributes_component.h"

using testing::ElementsAre;

namespace donner::svg::components {

TEST(AttributesComponent, FindMatchingAttributes) {
  AttributesComponent component;
  EXPECT_TRUE(component.findMatchingAttributes("test").empty());

  component.setAttribute("test", "value");
  component.setAttribute(XMLQualifiedNameRef("namespace", "test"), "value2");

  EXPECT_THAT(component.findMatchingAttributes("test"),  //
              ElementsAre("test"));
  EXPECT_THAT(component.findMatchingAttributes(XMLQualifiedNameRef("namespace", "test")),  //
              ElementsAre(                                                                 //
                  XMLQualifiedNameRef("namespace", "test")));
  EXPECT_THAT(component.findMatchingAttributes(XMLQualifiedNameRef("*", "test")),  //
              ElementsAre(                                                         //
                  "test",                                                          //
                  XMLQualifiedNameRef("namespace", "test")));
}

}  // namespace donner::svg::components
