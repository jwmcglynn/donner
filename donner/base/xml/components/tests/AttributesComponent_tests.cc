#include "donner/base/xml/components/AttributesComponent.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/TreeComponent.h"
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

  EXPECT_THAT(component.attributes(),
              ElementsAre(  //
                  "test",   //
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
  EXPECT_THAT(component.findMatchingAttributes(XMLQualifiedNameRef("*", "no-match")),  //
              ElementsAre());
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

TEST(AttributesComponent, NamespaceOverride) {
  Registry registry;
  auto& namespaceContext = registry.ctx().emplace<XMLNamespaceContext>(registry);

  Entity entity = registry.create();
  AttributesComponent& component = registry.emplace<AttributesComponent>(entity);

  // No namespace URI is the default
  EXPECT_THAT(namespaceContext.getNamespaceUri(registry, entity, ""), testing::Eq(std::nullopt));

  component.setAttribute(registry, XMLQualifiedNameRef("", "xmlns"), "https://example.com");
  EXPECT_EQ(namespaceContext.getNamespaceUri(registry, entity, ""), "https://example.com");

  component.setAttribute(registry, XMLQualifiedNameRef("xmlns", "test"),
                         "https://example.com/test");
  EXPECT_EQ(namespaceContext.getNamespaceUri(registry, entity, "test"), "https://example.com/test");

  component.removeAttribute(registry, XMLQualifiedNameRef("", "xmlns"));
  EXPECT_FALSE(namespaceContext.getNamespaceUri(registry, entity, ""));

  // Previous namespace is still there
  EXPECT_EQ(namespaceContext.getNamespaceUri(registry, entity, "test"), "https://example.com/test");

  // Changing the attribute replaces the namespace
  component.setAttribute(registry, XMLQualifiedNameRef("xmlns", "test"), "https://example.com/2");
  EXPECT_EQ(namespaceContext.getNamespaceUri(registry, entity, "test"), "https://example.com/2");

  // Removing the attribute removes the namespace
  component.removeAttribute(registry, XMLQualifiedNameRef("xmlns", "test"));
  EXPECT_FALSE(namespaceContext.getNamespaceUri(registry, entity, "test"));
}

TEST(AttributesComponent, NamespaceInTree) {
  Registry registry;
  auto& namespaceContext = registry.ctx().emplace<XMLNamespaceContext>(registry);

  Entity parent = registry.create();
  AttributesComponent& parentAttributes = registry.emplace<AttributesComponent>(parent);

  Entity child = registry.create();
  AttributesComponent& childAttributes = registry.emplace<AttributesComponent>(child);

  // Link the two together
  {
    auto& parentTree = registry.emplace<TreeComponent>(parent, "parent");
    registry.emplace<TreeComponent>(child, "child");
    parentTree.appendChild(registry, child);
  }

  // Set the default namespace of the parent, query on the child
  parentAttributes.setAttribute(registry, XMLQualifiedNameRef("", "xmlns"), "https://example.com");
  EXPECT_EQ(namespaceContext.getNamespaceUri(registry, child, ""), "https://example.com");

  // Override on the child
  childAttributes.setAttribute(registry, XMLQualifiedNameRef("", "xmlns"),
                               "https://example.com/child");
  EXPECT_EQ(namespaceContext.getNamespaceUri(registry, child, ""), "https://example.com/child");

  // Remove the override
  childAttributes.removeAttribute(registry, XMLQualifiedNameRef("", "xmlns"));
  EXPECT_EQ(namespaceContext.getNamespaceUri(registry, child, ""), "https://example.com");

  // Destroy the entity and check the parent
  registry.destroy(child);
  EXPECT_EQ(namespaceContext.getNamespaceUri(registry, parent, ""), "https://example.com");
}

TEST(AttributesComponent, NamespaceEntityDelete) {
  Registry registry;
  auto& namespaceContext = registry.ctx().emplace<XMLNamespaceContext>(registry);

  Entity parent = registry.create();
  AttributesComponent& parentAttributes = registry.emplace<AttributesComponent>(parent);

  Entity child = registry.create();
  AttributesComponent& childAttributes = registry.emplace<AttributesComponent>(child);

  // Link the two together
  {
    auto& parentTree = registry.emplace<TreeComponent>(parent, "parent");
    registry.emplace<TreeComponent>(child, "child");
    parentTree.appendChild(registry, child);
  }

  // Override on the child
  childAttributes.setAttribute(registry, XMLQualifiedNameRef("", "xmlns"),
                               "https://example.com/child");
  EXPECT_TRUE(childAttributes.hasNamespaceOverrides());
  EXPECT_EQ(namespaceContext.getNamespaceUri(registry, child, ""), "https://example.com/child");
  EXPECT_THAT(namespaceContext.getNamespaceUri(registry, parent, ""), testing::Eq(std::nullopt));

  // Override on the parent
  parentAttributes.setAttribute(registry, XMLQualifiedNameRef("", "xmlns"), "https://example.com");
  EXPECT_TRUE(parentAttributes.hasNamespaceOverrides());

  // Destroy the entity and check the parent
  registry.destroy(child);
  EXPECT_EQ(namespaceContext.getNamespaceUri(registry, parent, ""), "https://example.com");
}

}  // namespace donner::components
