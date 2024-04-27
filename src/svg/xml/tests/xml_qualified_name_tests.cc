#include <gtest/gtest.h>

#include "src/svg/xml/xml_qualified_name.h"

namespace donner::svg {

// TODO: Test coverage to add:
// - Constructors
// - Copy and move operators.
// - Cast operators.

TEST(XMLQualifiedNameTest, WorksInMap) {
  std::map<XMLQualifiedName, int> attrMap;

  attrMap[XMLQualifiedName("id")] = 1;
  attrMap[XMLQualifiedName("myNamespace", "data-count")] = 5;

  EXPECT_EQ(attrMap.size(), 2);
  EXPECT_EQ(attrMap[XMLQualifiedName("id")], 1);
  EXPECT_EQ(attrMap[XMLQualifiedName("myNamespace", "data-count")], 5);
}

TEST(XMLQualifiedNameTest, WorksInUnorderedMap) {
  std::unordered_map<XMLQualifiedName, int> attrMap;

  attrMap[XMLQualifiedName("", "id")] = 1;
  attrMap[XMLQualifiedName("myNamespace", "data-count")] = 5;

  EXPECT_EQ(attrMap.size(), 2);
  EXPECT_EQ(attrMap[XMLQualifiedName("", "id")], 1);
  EXPECT_EQ(attrMap[XMLQualifiedName("myNamespace", "data-count")], 5);
}

TEST(XMLQualifiedNameTest, ComparisonOperators) {
  const XMLQualifiedName attr1("", "class");
  const XMLQualifiedName attr2("", "href");

  EXPECT_TRUE(attr1 == attr1);
  EXPECT_FALSE(attr1 == attr2);

  EXPECT_FALSE(attr1 != attr1);
  EXPECT_TRUE(attr1 != attr2);

  EXPECT_TRUE(attr1 < attr2);
  EXPECT_FALSE(attr2 < attr1);
}

TEST(XMLQualifiedNameTest, ComparisonOperatorsWithNamespaces) {
  XMLQualifiedName xlinkHref("xlink", "href");
  XMLQualifiedName xlinkClass("xlink", "class");
  XMLQualifiedName svgHref("svg", "href");

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

TEST(XMLQualifiedNameTest, ComparisonOperatorsBetweenNamespacedAndNonNamespaced) {
  XMLQualifiedName href("", "href");
  XMLQualifiedNameRef href2("href");
  XMLQualifiedName xlinkHref("xlink", "href");
  XMLQualifiedNameRef xlinkHref2("xlink", "href");

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

TEST(XMLQualifiedNameRefTest, WorksInMap) {
  std::map<XMLQualifiedNameRef, int> attrMap;

  attrMap["class"] = 123;
  attrMap[XMLQualifiedNameRef("", "id")] = 1;
  attrMap[XMLQualifiedNameRef("myNamespace", "data-count")] = 5;

  EXPECT_EQ(attrMap.size(), 3);
  EXPECT_EQ(attrMap["class"], 123);
  EXPECT_EQ(attrMap[XMLQualifiedNameRef("", "id")], 1);
  EXPECT_EQ(attrMap[XMLQualifiedNameRef("myNamespace", "data-count")], 5);
}

TEST(XMLQualifiedNameRefTest, WorksInUnorderedMap) {
  std::unordered_map<XMLQualifiedNameRef, int> attrMap;

  attrMap["class"] = 123;
  attrMap[XMLQualifiedNameRef("", "id")] = 1;
  attrMap[XMLQualifiedNameRef("myNamespace", "data-count")] = 5;

  EXPECT_EQ(attrMap.size(), 3);
  EXPECT_EQ(attrMap["class"], 123);
  EXPECT_EQ(attrMap[XMLQualifiedNameRef("id")], 1);
  EXPECT_EQ(attrMap[XMLQualifiedNameRef("myNamespace", "data-count")], 5);
}

TEST(XMLQualifiedNameRefTest, ComparisonOperators) {
  const XMLQualifiedName attrClass("", "class");
  const XMLQualifiedNameRef attrClass2("class");
  const XMLQualifiedName attrHref("", "href");
  const XMLQualifiedNameRef attrHref2("href");

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

TEST(XMLQualifiedNameRefTest, ComparisonOperatorsWithNamespaces) {
  XMLQualifiedName xlinkHref("xlink", "href");
  XMLQualifiedNameRef xlinkHref2("xlink", "href");
  XMLQualifiedName xlinkClass("xlink", "class");
  XMLQualifiedNameRef xlinkClass2("xlink", "class");
  XMLQualifiedName svgHref("svg", "href");
  XMLQualifiedNameRef svgHref2("svg", "href");

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

TEST(XMLQualifiedNameRefTest, ComparisonOperatorsBetweenNamespacedAndNonNamespaced) {
  XMLQualifiedName href("", "href");
  XMLQualifiedNameRef href2("href");
  XMLQualifiedName xlinkHref("xlink", "href");
  XMLQualifiedNameRef xlinkHref2("xlink", "href");

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
