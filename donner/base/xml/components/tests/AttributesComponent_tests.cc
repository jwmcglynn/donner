#include "donner/base/xml/components/AttributesComponent.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"

using testing::ElementsAre;

namespace donner::components {

using xml::XMLQualifiedNameRef;
using xml::components::XMLNamespaceContext;

TEST(AttributesComponent, HasAttribute) {
  Registry registry;
  Entity entity = registry.create();
  AttributesComponent& component = registry.emplace<AttributesComponent>(entity);

  EXPECT_FALSE(component.hasAttribute("test"));
  EXPECT_FALSE(component.hasAttribute(XMLQualifiedNameRef("namespace", "test")));

  component.setAttribute(registry, "test", "value");
  component.setAttribute(registry, XMLQualifiedNameRef("namespace", "test"), "value2");

  EXPECT_TRUE(component.hasAttribute("test"));
  EXPECT_TRUE(component.hasAttribute(XMLQualifiedNameRef("namespace", "test")));
}

TEST(AttributesComponent, GetAttribute) {
  Registry registry;
  registry.ctx().emplace<XMLNamespaceContext>(registry);

  Entity entity = registry.create();
  AttributesComponent& component = registry.emplace<AttributesComponent>(entity);

  EXPECT_FALSE(component.getAttribute("test").has_value());
  EXPECT_FALSE(component.getAttribute(XMLQualifiedNameRef("namespace", "test")).has_value());

  component.setAttribute(registry, "test", "value");
  component.setAttribute(registry, XMLQualifiedNameRef("namespace", "test"), "value2");

  EXPECT_EQ(component.getAttribute("test"), "value");
  EXPECT_EQ(component.getAttribute(XMLQualifiedNameRef("namespace", "test")), "value2");
}

TEST(AttributesComponent, Attributes) {
  Registry registry;
  registry.ctx().emplace<XMLNamespaceContext>(registry);

  Entity entity = registry.create();
  AttributesComponent& component = registry.emplace<AttributesComponent>(entity);

  EXPECT_TRUE(component.attributes().empty());

  component.setAttribute(registry, "test", "value");
  component.setAttribute(registry, XMLQualifiedNameRef("namespace", "test"), "value2");

  EXPECT_THAT(component.attributes(),  //
              ElementsAre(             //
                  "test",              //
                  XMLQualifiedNameRef("namespace", "test")));
}

TEST(AttributesComponent, FindMatchingAttributes) {
  Registry registry;
  registry.ctx().emplace<XMLNamespaceContext>(registry);

  Entity entity = registry.create();
  AttributesComponent& component = registry.emplace<AttributesComponent>(entity);

  EXPECT_TRUE(component.findMatchingAttributes("test").empty());

  component.setAttribute(registry, "test", "value");
  component.setAttribute(registry, XMLQualifiedNameRef("namespace", "test"), "value2");

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

TEST(AttributesComponent, SetAttribute) {
  Registry registry;
  registry.ctx().emplace<XMLNamespaceContext>(registry);

  Entity entity = registry.create();
  AttributesComponent& component = registry.emplace<AttributesComponent>(entity);

  component.setAttribute(registry, "test", "value");
  component.setAttribute(registry, XMLQualifiedNameRef("namespace", "test"), "value2");

  EXPECT_EQ(component.getAttribute("test"), "value");
  EXPECT_EQ(component.getAttribute(XMLQualifiedNameRef("namespace", "test")), "value2");

  // Override an existing value.
  component.setAttribute(registry, "test", "value3");
  EXPECT_EQ(component.getAttribute("test"), "value3");
}

TEST(AttributesComponent, RemoveAttribute) {
  Registry registry;
  registry.ctx().emplace<XMLNamespaceContext>(registry);

  Entity entity = registry.create();
  AttributesComponent& component = registry.emplace<AttributesComponent>(entity);

  component.setAttribute(registry, "test", "value");
  component.setAttribute(registry, XMLQualifiedNameRef("namespace", "test"), "value2");

  component.removeAttribute(registry, "test");
  EXPECT_FALSE(component.hasAttribute("test"));

  component.removeAttribute(registry, XMLQualifiedNameRef("namespace", "test"));
  EXPECT_FALSE(component.hasAttribute(XMLQualifiedNameRef("namespace", "test")));
}

}  // namespace donner::components
