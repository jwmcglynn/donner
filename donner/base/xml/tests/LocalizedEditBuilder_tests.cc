#include "donner/base/xml/LocalizedEditBuilder.h"

#include "donner/base/FileOffset.h"
#include "donner/base/xml/SourceDocument.h"
#include "donner/base/xml/XMLParser.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::xml {
namespace {

using ::testing::Optional;

class LocalizedEditBuilderTests : public ::testing::Test {
protected:
  XMLDocument parse(std::string_view xml) {
    auto result = XMLParser::Parse(xml);
    EXPECT_TRUE(result.hasResult());
    return std::move(result.result());
  }
};

std::optional<XMLNode> findFirstElement(const XMLNode& parent) {
  for (auto child = parent.firstChild(); child.has_value(); child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Element) {
      return child;
    }
  }

  return std::nullopt;
}

TEST_F(LocalizedEditBuilderTests, InsertBeforeSiblingUsesSiblingIndentation) {
  constexpr std::string_view kSource = "<svg>\n  <rect id=\"a\"/>\n</svg>";
  XMLDocument document = parse(kSource);
  XMLNode root = document.root();
  auto svg = findFirstElement(root);
  ASSERT_THAT(svg, Optional(::testing::Truly(
                       [](const XMLNode& node) { return node.tagName().toString() == "svg"; })));

  auto rect = findFirstElement(*svg);
  ASSERT_THAT(rect, Optional(::testing::Truly(
                        [](const XMLNode& node) { return node.tagName().toString() == "rect"; })));

  XMLNode circle = XMLNode::CreateElementNode(document, XMLQualifiedNameRef("circle"));
  circle.setAttribute(XMLQualifiedNameRef("fill"), "red");

  LocalizedEditBuilder builder(kSource);
  auto replacement = builder.insertBeforeSibling(circle, *rect);
  ASSERT_TRUE(replacement.has_value());

  SourceDocument sourceDoc{RcString(kSource)};
  auto applied = sourceDoc.applyReplacements({replacement.value()});
  ASSERT_TRUE(applied.hasResult());
  EXPECT_EQ(std::string_view(applied.result().text),
            "<svg>\n  <circle fill=\"red\"/>\n  <rect id=\"a\"/>\n</svg>");
}

TEST_F(LocalizedEditBuilderTests, AppendChildAnchorsBeforeClosingTag) {
  constexpr std::string_view kSource = "<svg>\n</svg>";
  XMLDocument document = parse(kSource);
  XMLNode root = document.root();
  auto svg = findFirstElement(root);
  ASSERT_TRUE(svg.has_value());

  XMLNode rect = XMLNode::CreateElementNode(document, XMLQualifiedNameRef("rect"));
  rect.setAttribute(XMLQualifiedNameRef("id"), "a");

  LocalizedEditBuilder builder(kSource);
  auto replacement = builder.appendChild(rect, *svg);
  ASSERT_TRUE(replacement.has_value());

  SourceDocument sourceDoc{RcString(kSource)};
  auto applied = sourceDoc.applyReplacements({replacement.value()});
  ASSERT_TRUE(applied.hasResult());
  EXPECT_EQ(std::string_view(applied.result().text), "<svg>\n<rect id=\"a\"/>\n</svg>");
}

TEST_F(LocalizedEditBuilderTests, RemoveNodeUsesRecordedSpan) {
  constexpr std::string_view kSource = "<svg><rect id=\"a\"/></svg>";
  XMLDocument document = parse(kSource);
  XMLNode root = document.root();
  auto svg = findFirstElement(root);
  ASSERT_TRUE(svg.has_value());
  auto rect = findFirstElement(*svg);
  ASSERT_TRUE(rect.has_value());

  LocalizedEditBuilder builder(kSource);
  auto replacement = builder.removeNode(*rect);
  ASSERT_TRUE(replacement.has_value());
  EXPECT_TRUE(replacement->range.start.offset.has_value());
  EXPECT_TRUE(replacement->range.end.offset.has_value());

  SourceDocument sourceDoc{RcString(kSource)};
  auto applied = sourceDoc.applyReplacements({replacement.value()});
  ASSERT_TRUE(applied.hasResult());
  EXPECT_EQ(std::string_view(applied.result().text), "<svg></svg>");
}

}  // namespace
}  // namespace donner::xml
