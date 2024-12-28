#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/RcString.h"
#include "donner/base/parser/ParseResult.h"
#include "donner/base/parser/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"

using namespace donner::base::parser;
using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;
using testing::Ne;

// TODO: Add an ErrorHighlightsText matcher

namespace donner::xml {

class XMLNodeTests : public testing::Test {
protected:
  /**
   * Parse an XML string and return the first node.
   */
  std::optional<XMLNode> parseAndGetFirstNode(std::string_view xml,
                                              XMLParser::Options options = {}) {
    SCOPED_TRACE(testing::Message() << "Parsing XML:\n" << xml << "\n\n");

    ParseResult<XMLDocument> maybeDocument = XMLParser::Parse(xml, options);
    EXPECT_THAT(maybeDocument, NoParseError());
    if (maybeDocument.hasError()) {
      return std::nullopt;
    }

    document_ = std::move(maybeDocument.result());
    XMLNode root = document_.root();
    EXPECT_EQ(root.type(), XMLNode::Type::Document);
    EXPECT_THAT(root.nextSibling(), Eq(std::nullopt))
        << "XML must contain only a single element, such as <node></node>";

    return root.firstChild();
  }

private:
  XMLDocument document_;
};

TEST_F(XMLNodeTests, GetNodeLocation) {
  std::string_view xml = R"(<root><child attr="Hello, world!"></child></root>)";

  auto maybeRoot = parseAndGetFirstNode(xml);
  ASSERT_TRUE(maybeRoot.has_value());

  XMLNode root = std::move(maybeRoot.value());
  {
    EXPECT_EQ(root.type(), XMLNode::Type::Element);
    EXPECT_THAT(root.tagName(), Eq("root"));

    auto location = root.getNodeLocation();

    // Extract substring from the returned offsets.
    const size_t start = location->start.offset.value();
    const size_t end = location->end.offset.value();
    EXPECT_LT(start, end);
    std::string_view nodeSubstr = xml.substr(start, end - start);

    EXPECT_THAT(nodeSubstr, Eq(xml));
  }

  auto maybeChild = root.firstChild();
  ASSERT_TRUE(maybeChild.has_value());

  XMLNode child = std::move(maybeChild.value());
  {
    EXPECT_EQ(child.type(), XMLNode::Type::Element);
    EXPECT_THAT(child.tagName(), Eq("child"));

    auto location = root.getNodeLocation();
    ASSERT_THAT(location, Ne(std::nullopt));

    // Extract substring from the returned offsets.
    const size_t start = location->start.offset.value();
    const size_t end = location->end.offset.value();
    EXPECT_LT(start, end);
    std::string_view nodeSubstr = xml.substr(start, end - start);

    EXPECT_THAT(nodeSubstr, Eq(xml));
  }
}

TEST_F(XMLNodeTests, GetAttributeLocation) {
  // Setup test XML.
  std::string_view xml = R"(<child attr="Hello, world!"></child>)";

  auto maybeChild = parseAndGetFirstNode(xml);
  ASSERT_TRUE(maybeChild.has_value());

  XMLNode child = std::move(maybeChild.value());
  auto location = child.getAttributeLocation(xml, "attr");
  ASSERT_THAT(location, Ne(std::nullopt));

  // Extract substring from the returned offsets.
  const size_t start = location->start.offset.value();
  const size_t end = location->end.offset.value();
  EXPECT_LT(start, end);
  std::string_view foundAttribute = xml.substr(start, end - start);

  // Verify correctness.
  EXPECT_THAT(foundAttribute, testing::Eq(R"(attr="Hello, world!")"));
}

}  // namespace donner::xml
