#include <gtest/gtest.h>

#include "src/svg/xml/xml_attribute.h"

namespace donner::svg {

// TODO: Test coverage to add:
// - Constructors
// - Copy and move operators.
// - Cast operators.

TEST(XMLAttributeTest, WorksInMap) {
  std::map<XMLAttribute, int> attrMap;

  attrMap[XMLAttribute("id")] = 1;
  attrMap[XMLAttribute("myNamespace", "data-count")] = 5;

  EXPECT_EQ(attrMap.size(), 2);
  EXPECT_EQ(attrMap[XMLAttribute("id")], 1);
  EXPECT_EQ(attrMap[XMLAttribute("myNamespace", "data-count")], 5);
}

TEST(XMLAttributeTest, WorksInUnorderedMap) {
  std::unordered_map<XMLAttribute, int> attrMap;

  attrMap[XMLAttribute("", "id")] = 1;
  attrMap[XMLAttribute("myNamespace", "data-count")] = 5;

  EXPECT_EQ(attrMap.size(), 2);
  EXPECT_EQ(attrMap[XMLAttribute("", "id")], 1);
  EXPECT_EQ(attrMap[XMLAttribute("myNamespace", "data-count")], 5);
}

TEST(XMLAttributeTest, ComparisonOperators) {
  const XMLAttribute attr1("", "class");
  const XMLAttribute attr2("", "href");

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
  XMLAttribute href("", "href");
  XMLAttributeRef href2("href");
  XMLAttribute xlinkHref("xlink", "href");
  XMLAttributeRef xlinkHref2("xlink", "href");

  EXPECT_FALSE(href == xlinkHref);
  EXPECT_FALSE(href2 == xlinkHref2);
  EXPECT_FALSE(href == xlinkHref2);
  EXPECT_FALSE(href2 == xlinkHref);

  EXPECT_TRUE(href != xlinkHref);
  EXPECT_TRUE(href2 != xlinkHref2);
  EXPECT_TRUE(href != xlinkHref2);
  EXPECT_TRUE(href2 != xlinkHref);

  EXPECT_TRUE(href < xlinkHref);
  EXPECT_TRUE(href2 < xlinkHref2);
  EXPECT_TRUE(href < xlinkHref2);
  EXPECT_TRUE(href2 < xlinkHref);

  EXPECT_FALSE(xlinkHref < href);
  EXPECT_FALSE(xlinkHref2 < href2);
  EXPECT_FALSE(xlinkHref2 < href);
  EXPECT_FALSE(xlinkHref < href2);
}

TEST(XMLAttributeRefTest, WorksInMap) {
  std::map<XMLAttributeRef, int> attrMap;

  attrMap["class"] = 123;
  attrMap[XMLAttributeRef("", "id")] = 1;
  attrMap[XMLAttributeRef("myNamespace", "data-count")] = 5;

  EXPECT_EQ(attrMap.size(), 3);
  EXPECT_EQ(attrMap["class"], 123);
  EXPECT_EQ(attrMap[XMLAttributeRef("", "id")], 1);
  EXPECT_EQ(attrMap[XMLAttributeRef("myNamespace", "data-count")], 5);
}

TEST(XMLAttributeRefTest, WorksInUnorderedMap) {
  std::unordered_map<XMLAttributeRef, int> attrMap;

  attrMap["class"] = 123;
  attrMap[XMLAttributeRef("", "id")] = 1;
  attrMap[XMLAttributeRef("myNamespace", "data-count")] = 5;

  EXPECT_EQ(attrMap.size(), 3);
  EXPECT_EQ(attrMap["class"], 123);
  EXPECT_EQ(attrMap[XMLAttributeRef("id")], 1);
  EXPECT_EQ(attrMap[XMLAttributeRef("myNamespace", "data-count")], 5);
}

TEST(XMLAttributeRefTest, ComparisonOperators) {
  const XMLAttribute attrClass("", "class");
  const XMLAttributeRef attrClass2("class");
  const XMLAttribute attrHref("", "href");
  const XMLAttributeRef attrHref2("href");

  EXPECT_TRUE(attrClass == attrClass);
  EXPECT_TRUE(attrClass2 == attrClass2);
  EXPECT_TRUE(attrClass == attrClass2);
  EXPECT_TRUE(attrClass2 == attrClass);

  EXPECT_FALSE(attrClass == attrHref);
  EXPECT_FALSE(attrClass2 == attrHref2);
  EXPECT_FALSE(attrClass == attrHref2);
  EXPECT_FALSE(attrClass2 == attrHref);

  EXPECT_TRUE(attrClass != attrHref);
  EXPECT_TRUE(attrClass2 != attrHref2);
  EXPECT_TRUE(attrClass != attrHref2);
  EXPECT_TRUE(attrClass2 != attrHref);

  EXPECT_FALSE(attrClass != attrClass);
  EXPECT_FALSE(attrClass2 != attrClass2);
  EXPECT_FALSE(attrClass != attrClass2);
  EXPECT_FALSE(attrClass2 != attrClass);

  EXPECT_TRUE(attrClass != attrHref);
  EXPECT_TRUE(attrClass2 != attrHref2);
  EXPECT_TRUE(attrClass != attrHref2);
  EXPECT_TRUE(attrClass2 != attrHref);

  EXPECT_TRUE(attrClass < attrHref);
  EXPECT_TRUE(attrClass2 < attrHref2);
  EXPECT_TRUE(attrClass < attrHref2);
  EXPECT_TRUE(attrClass2 < attrHref);

  EXPECT_FALSE(attrHref < attrClass);
  EXPECT_FALSE(attrHref2 < attrClass2);
  EXPECT_FALSE(attrHref2 < attrClass);
  EXPECT_FALSE(attrHref < attrClass2);
}

TEST(XMLAttributeRefTest, ComparisonOperatorsWithNamespaces) {
  XMLAttribute xlinkHref("xlink", "href");
  XMLAttributeRef xlinkHref2("xlink", "href");
  XMLAttribute xlinkClass("xlink", "class");
  XMLAttributeRef xlinkClass2("xlink", "class");
  XMLAttribute svgHref("svg", "href");
  XMLAttributeRef svgHref2("svg", "href");

  EXPECT_TRUE(xlinkHref == xlinkHref);
  EXPECT_TRUE(xlinkHref2 == xlinkHref2);
  EXPECT_TRUE(xlinkHref == xlinkHref2);
  EXPECT_TRUE(xlinkHref2 == xlinkHref);

  EXPECT_FALSE(xlinkHref == xlinkClass);
  EXPECT_FALSE(xlinkHref2 == xlinkClass2);
  EXPECT_FALSE(xlinkHref == xlinkClass2);
  EXPECT_FALSE(xlinkHref2 == xlinkClass);

  EXPECT_FALSE(xlinkHref == svgHref);
  EXPECT_FALSE(xlinkHref2 == svgHref2);
  EXPECT_FALSE(xlinkHref == svgHref2);
  EXPECT_FALSE(xlinkHref2 == svgHref);

  EXPECT_FALSE(xlinkHref != xlinkHref);
  EXPECT_FALSE(xlinkHref2 != xlinkHref2);
  EXPECT_FALSE(xlinkHref != xlinkHref2);
  EXPECT_FALSE(xlinkHref2 != xlinkHref);

  EXPECT_TRUE(xlinkHref != xlinkClass);
  EXPECT_TRUE(xlinkHref2 != xlinkClass2);
  EXPECT_TRUE(xlinkHref != xlinkClass2);
  EXPECT_TRUE(xlinkHref2 != xlinkClass);

  EXPECT_TRUE(xlinkHref != svgHref);
  EXPECT_TRUE(xlinkHref2 != svgHref2);
  EXPECT_TRUE(xlinkHref != svgHref2);
  EXPECT_TRUE(xlinkHref2 != svgHref);

  EXPECT_FALSE(xlinkHref < xlinkClass);
  EXPECT_FALSE(xlinkHref2 < xlinkClass2);
  EXPECT_FALSE(xlinkHref < xlinkClass2);
  EXPECT_FALSE(xlinkHref2 < xlinkClass);

  EXPECT_TRUE(xlinkClass < xlinkHref);
  EXPECT_TRUE(xlinkClass2 < xlinkHref2);
  EXPECT_TRUE(xlinkClass < xlinkHref2);
  EXPECT_TRUE(xlinkClass2 < xlinkHref);

  EXPECT_FALSE(xlinkHref < svgHref);
  EXPECT_FALSE(xlinkHref2 < svgHref2);
  EXPECT_FALSE(xlinkHref < svgHref2);
  EXPECT_FALSE(xlinkHref2 < svgHref);
}

TEST(XMLAttributeRefTest, ComparisonOperatorsBetweenNamespacedAndNonNamespaced) {
  XMLAttribute href("", "href");
  XMLAttributeRef href2("href");
  XMLAttribute xlinkHref("xlink", "href");
  XMLAttributeRef xlinkHref2("xlink", "href");

  EXPECT_FALSE(href == xlinkHref);
  EXPECT_FALSE(href2 == xlinkHref2);
  EXPECT_FALSE(href == xlinkHref2);
  EXPECT_FALSE(href2 == xlinkHref);

  EXPECT_TRUE(href != xlinkHref);
  EXPECT_TRUE(href2 != xlinkHref2);
  EXPECT_TRUE(href != xlinkHref2);
  EXPECT_TRUE(href2 != xlinkHref);

  EXPECT_TRUE(href < xlinkHref);
  EXPECT_TRUE(href2 < xlinkHref2);
  EXPECT_TRUE(href < xlinkHref2);
  EXPECT_TRUE(href2 < xlinkHref);

  EXPECT_FALSE(xlinkHref < href);
  EXPECT_FALSE(xlinkHref2 < href2);
  EXPECT_FALSE(xlinkHref2 < href);
  EXPECT_FALSE(xlinkHref < href2);
}

}  // namespace donner::svg
