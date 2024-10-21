#include "donner/base/xml/XMLQualifiedName.h"

#include <gtest/gtest.h>

namespace donner::xml {

TEST(XMLQualifiedNameTest, Constructors) {
  // Constructor with only name
  XMLQualifiedName nameOnly("testName");
  EXPECT_TRUE(nameOnly.namespacePrefix.empty());
  EXPECT_EQ(nameOnly.name, "testName");

  // Constructor with namespace and name
  XMLQualifiedName fullName("testNamespace", "testName");
  EXPECT_EQ(fullName.namespacePrefix, "testNamespace");
  EXPECT_EQ(fullName.name, "testName");
}

TEST(XMLQualifiedNameTest, CopyAndMoveOperators) {
  XMLQualifiedName original("testNamespace", "testName");

  // Copy constructor
  XMLQualifiedName copied(original);
  EXPECT_EQ(copied.namespacePrefix, "testNamespace");
  EXPECT_EQ(copied.name, "testName");

  // Move constructor
  XMLQualifiedName moved(std::move(copied));
  EXPECT_EQ(moved.namespacePrefix, "testNamespace");
  EXPECT_EQ(moved.name, "testName");
  // Note: We can't make assumptions about the state of 'copied' after move

  // Copy assignment
  XMLQualifiedName copyAssigned("empty");
  copyAssigned = original;
  EXPECT_EQ(copyAssigned.namespacePrefix, "testNamespace");
  EXPECT_EQ(copyAssigned.name, "testName");

  // Move assignment
  XMLQualifiedName moveAssigned("empty");
  moveAssigned = std::move(copyAssigned);
  EXPECT_EQ(moveAssigned.namespacePrefix, "testNamespace");
  EXPECT_EQ(moveAssigned.name, "testName");
  // Note: We can't make assumptions about the state of 'copyAssigned' after move
}

TEST(XMLQualifiedNameTest, CastOperators) {
  XMLQualifiedName original("testNamespace", "testName");

  // Cast to XMLQualifiedNameRef
  XMLQualifiedNameRef asRef = original;
  EXPECT_EQ(asRef.namespacePrefix, "testNamespace");
  EXPECT_EQ(asRef.name, "testName");
}

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

TEST(XMLQualifiedNameTest, OutputOperators) {
  XMLQualifiedName withNamespace("testNamespace", "testName");
  XMLQualifiedName noNamespace("", "testName");

  // .toString()
  EXPECT_EQ(withNamespace.toString(), "testNamespace:testName");
  EXPECT_EQ(noNamespace.toString(), "testName");

  // Test with std::ostream
  std::ostringstream stream;
  stream << withNamespace;
  EXPECT_EQ(stream.str(), "testNamespace:testName");

  stream.str("");
  stream << noNamespace;
  EXPECT_EQ(stream.str(), "testName");

  // Test with printCssSyntax()
  stream.str("");
  stream << withNamespace.printCssSyntax();
  EXPECT_EQ(stream.str(), "testNamespace|testName");

  stream.str("");
  stream << noNamespace.printCssSyntax();
  EXPECT_EQ(stream.str(), "testName");
}

TEST(XMLQualifiedNameRefTest, Constructors) {
  // Constructor with only name as RcStringOrRef
  XMLQualifiedNameRef nameOnly1(RcStringOrRef("testName"));
  EXPECT_TRUE(nameOnly1.namespacePrefix.empty());
  EXPECT_EQ(nameOnly1.name, "testName");

  // Constructor with only name as const char*
  XMLQualifiedNameRef nameOnly2("testName");
  EXPECT_TRUE(nameOnly2.namespacePrefix.empty());
  EXPECT_EQ(nameOnly2.name, "testName");

  // Constructor with only name as std::string_view
  std::string_view nameView("testName");
  XMLQualifiedNameRef nameOnly3(nameView);
  EXPECT_TRUE(nameOnly3.namespacePrefix.empty());
  EXPECT_EQ(nameOnly3.name, "testName");

  // Constructor with namespace and name as RcStringOrRef
  XMLQualifiedNameRef fullName1(RcStringOrRef("testNamespace"), RcStringOrRef("testName"));
  EXPECT_EQ(fullName1.namespacePrefix, "testNamespace");
  EXPECT_EQ(fullName1.name, "testName");

  // Constructor with namespace and name as const char*
  XMLQualifiedNameRef fullName2("testNamespace", "testName");
  EXPECT_EQ(fullName2.namespacePrefix, "testNamespace");
  EXPECT_EQ(fullName2.name, "testName");

  // Constructor from XMLQualifiedName
  XMLQualifiedName qualifiedName("testNamespace", "testName");
  XMLQualifiedNameRef fromQualifiedName(qualifiedName);
  EXPECT_EQ(fromQualifiedName.namespacePrefix, "testNamespace");
  EXPECT_EQ(fromQualifiedName.name, "testName");

  // Test with empty namespace
  XMLQualifiedNameRef emptyNamespace("", "testName");
  EXPECT_TRUE(emptyNamespace.namespacePrefix.empty());
  EXPECT_EQ(emptyNamespace.name, "testName");
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

TEST(XMLQualifiedNameRefTest, OutputOperators) {
  XMLQualifiedNameRef withNamespace("testNamespace", "testName");
  XMLQualifiedNameRef noNamespace("", "testName");

  // .toString()
  EXPECT_EQ(withNamespace.toString(), "testNamespace:testName");
  EXPECT_EQ(noNamespace.toString(), "testName");

  // Test with std::ostream
  std::ostringstream stream;
  stream << withNamespace;
  EXPECT_EQ(stream.str(), "testNamespace:testName");

  stream.str("");
  stream << noNamespace;
  EXPECT_EQ(stream.str(), "testName");

  // Test with printCssSyntax()
  stream.str("");
  stream << withNamespace.printCssSyntax();
  EXPECT_EQ(stream.str(), "testNamespace|testName");

  stream.str("");
  stream << noNamespace.printCssSyntax();
  EXPECT_EQ(stream.str(), "testName");
}

}  // namespace donner::xml
