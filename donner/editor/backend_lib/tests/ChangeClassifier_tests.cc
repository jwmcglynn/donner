#include "donner/editor/backend_lib/ChangeClassifier.h"

#include <gtest/gtest.h>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {
namespace {

constexpr std::string_view kSimpleSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <rect id="r1" x="10" y="20" width="50" height="30" fill="red"/>
</svg>)";

class ChangeClassifierTest : public ::testing::Test {
protected:
  void SetUp() override {
    ParseWarningSink sink;
    auto result = svg::parser::SVGParser::ParseSVG(kSimpleSvg, sink);
    ASSERT_TRUE(result.hasResult());
    document_ = std::move(result.result());
  }

  svg::SVGDocument document_;
};

TEST_F(ChangeClassifierTest, EditInsideAttributeValueClassifiesAsSetAttribute) {
  // Change fill="red" to fill="blue" — the change is entirely inside the
  // quoted attribute value.
  std::string newSource(kSimpleSvg);
  const auto fillPos = newSource.find("\"red\"");
  ASSERT_NE(fillPos, std::string::npos);
  newSource.replace(fillPos + 1, 3, "blue");

  auto result = classifyTextChange(document_, kSimpleSvg, newSource);
  ASSERT_TRUE(result.command.has_value());
  EXPECT_EQ(result.command->kind, EditorCommand::Kind::SetAttribute);
  EXPECT_EQ(result.command->attributeName, "fill");
  EXPECT_EQ(result.command->attributeValue, "blue");
}

TEST_F(ChangeClassifierTest, EditOutsideAttributeValueIsStructural) {
  // Insert a character outside any attribute value — e.g. before the <rect.
  std::string newSource(kSimpleSvg);
  const auto rectPos = newSource.find("<rect");
  ASSERT_NE(rectPos, std::string::npos);
  newSource.insert(rectPos, "X");

  auto result = classifyTextChange(document_, kSimpleSvg, newSource);
  EXPECT_FALSE(result.command.has_value()) << "Expected structural fallback";
}

TEST_F(ChangeClassifierTest, EditThatDeletesQuoteIsStructural) {
  // Delete the closing quote of fill="red" → structural because the
  // attribute value is no longer well-formed.
  std::string newSource(kSimpleSvg);
  const auto fillPos = newSource.find("fill=\"red\"");
  ASSERT_NE(fillPos, std::string::npos);
  // Remove the closing '"' (9 chars into "fill=\"red\"")
  newSource.erase(fillPos + 9, 1);

  auto result = classifyTextChange(document_, kSimpleSvg, newSource);
  EXPECT_FALSE(result.command.has_value()) << "Expected structural fallback";
}

TEST_F(ChangeClassifierTest, IdenticalSourceProducesNoCommand) {
  auto result = classifyTextChange(document_, kSimpleSvg, kSimpleSvg);
  EXPECT_FALSE(result.command.has_value());
}

TEST_F(ChangeClassifierTest, EditWidthAttribute) {
  // Change width="50" to width="100".
  std::string newSource(kSimpleSvg);
  const auto widthPos = newSource.find("width=\"50\"");
  ASSERT_NE(widthPos, std::string::npos);
  newSource.replace(widthPos + 7, 2, "100");

  auto result = classifyTextChange(document_, kSimpleSvg, newSource);
  ASSERT_TRUE(result.command.has_value());
  EXPECT_EQ(result.command->kind, EditorCommand::Kind::SetAttribute);
  EXPECT_EQ(result.command->attributeName, "width");
  EXPECT_EQ(result.command->attributeValue, "100");
}

TEST_F(ChangeClassifierTest, InsertCharacterIntoAttributeValue) {
  // Change fill="red" to fill="redd" — an insertion inside the value.
  std::string newSource(kSimpleSvg);
  const auto redPos = newSource.find("\"red\"");
  ASSERT_NE(redPos, std::string::npos);
  newSource.insert(redPos + 3, "d");

  auto result = classifyTextChange(document_, kSimpleSvg, newSource);
  ASSERT_TRUE(result.command.has_value());
  EXPECT_EQ(result.command->attributeValue, "redd");
}

TEST_F(ChangeClassifierTest, DeleteCharacterFromAttributeValue) {
  // Change fill="red" to fill="re" — a deletion inside the value.
  std::string newSource(kSimpleSvg);
  const auto redPos = newSource.find("\"red\"");
  ASSERT_NE(redPos, std::string::npos);
  newSource.erase(redPos + 3, 1);

  auto result = classifyTextChange(document_, kSimpleSvg, newSource);
  ASSERT_TRUE(result.command.has_value());
  EXPECT_EQ(result.command->attributeValue, "re");
}

}  // namespace
}  // namespace donner::editor
