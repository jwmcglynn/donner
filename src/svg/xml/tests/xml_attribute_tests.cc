#include <gtest/gtest.h>

#include "src/svg/xml/xml_attribute.h"

namespace donner::svg {

TEST(XMLAttributeTest, WorksInMap) {
  std::map<XMLAttribute, int> attrMap;

  attrMap["class"] = 123;
  attrMap[XMLAttribute("id")] = 1;
  attrMap[XMLAttribute("myNamespace", "data-count")] = 5;

  EXPECT_EQ(attrMap.size(), 3);
  EXPECT_EQ(attrMap["class"], 123);
  EXPECT_EQ(attrMap[XMLAttribute("id")], 1);
  EXPECT_EQ(attrMap[XMLAttribute("myNamespace", "data-count")], 5);
}

TEST(XMLAttributeTest, WorksInUnorderedMap) {
  std::unordered_map<XMLAttribute, int> attrMap;

  attrMap["class"] = 123;
  attrMap[XMLAttribute("id")] = 1;
  attrMap[XMLAttribute("myNamespace", "data-count")] = 5;

  EXPECT_EQ(attrMap.size(), 3);
  EXPECT_EQ(attrMap["class"], 123);
  EXPECT_EQ(attrMap[XMLAttribute("id")], 1);
  EXPECT_EQ(attrMap[XMLAttribute("myNamespace", "data-count")], 5);
}

TEST(XMLAttributeTest, ComparisonOperators) {
  const XMLAttribute attr1("class");
  const XMLAttribute attr2("href");

  EXPECT_TRUE(attr1 == attr1);
  EXPECT_FALSE(attr1 == attr2);

  EXPECT_FALSE(attr1 != attr1);
  EXPECT_TRUE(attr1 != attr2);

  EXPECT_TRUE(attr1 < attr2);
  EXPECT_FALSE(attr2 < attr1);
}

TEST(XMLAttributeTest, ComparisonOperatorsWithNamespaces) {
  XMLAttribute xlinkHref("xlink", "href");
  XMLAttribute xlinkClass("xlink", "class");
  XMLAttribute svgHref("svg", "href");

  EXPECT_TRUE(xlinkHref == xlinkHref);
  EXPECT_FALSE(xlinkHref == xlinkClass);
  EXPECT_FALSE(xlinkHref == svgHref);

  EXPECT_FALSE(xlinkHref != xlinkHref);
  EXPECT_TRUE(xlinkHref != xlinkClass);
  EXPECT_TRUE(xlinkHref != svgHref);

  EXPECT_FALSE(xlinkHref < xlinkClass);
  EXPECT_TRUE(xlinkClass < xlinkHref);
  EXPECT_FALSE(xlinkHref < svgHref);
}

TEST(XMLAttributeTest, ComparisonOperatorsBetweenNamespacedAndNonNamespaced) {
  XMLAttribute href("href");
  XMLAttribute xlinkHref("xlink", "href");

  EXPECT_FALSE(href == xlinkHref);
  EXPECT_TRUE(href != xlinkHref);
  EXPECT_TRUE(href < xlinkHref);
  EXPECT_FALSE(xlinkHref < href);
}

}  // namespace donner::svg
