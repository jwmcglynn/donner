#include "donner/editor/XmlAutocomplete.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>
#include <vector>

namespace donner::editor {

namespace {

bool HasSuggestion(const std::vector<XmlAutocompleteSuggestion>& suggestions,
                   std::string_view displayText) {
  return std::any_of(suggestions.begin(), suggestions.end(),
                     [displayText](const XmlAutocompleteSuggestion& suggestion) {
                       return suggestion.displayText == displayText;
                     });
}

const XmlAutocompleteSuggestion* FindSuggestion(
    const std::vector<XmlAutocompleteSuggestion>& suggestions, std::string_view displayText) {
  const auto it = std::find_if(suggestions.begin(), suggestions.end(),
                               [displayText](const XmlAutocompleteSuggestion& suggestion) {
                                 return suggestion.displayText == displayText;
                               });
  return it == suggestions.end() ? nullptr : &*it;
}

}  // namespace

TEST(XmlAutocomplete, SuggestsElementsAfterTagOpen) {
  constexpr std::string_view kSource = "<svg><";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::ElementName);
  EXPECT_EQ(context.replaceStartOffset, kSource.size());
  EXPECT_EQ(context.replaceEndOffset, kSource.size());

  const std::vector<XmlAutocompleteSuggestion> suggestions =
      BuildXmlAutocompleteSuggestions(context);
  EXPECT_TRUE(HasSuggestion(suggestions, "rect"));
  EXPECT_TRUE(HasSuggestion(suggestions, "linearGradient"));
}

TEST(XmlAutocomplete, ReplacesPartialElementName) {
  constexpr std::string_view kSource = "<svg><re";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::ElementName);
  EXPECT_EQ(context.replaceStartOffset, 6u);
  EXPECT_EQ(context.replaceEndOffset, 8u);
  EXPECT_EQ(context.prefix, "re");

  const std::vector<XmlAutocompleteSuggestion> suggestions =
      BuildXmlAutocompleteSuggestions(context);
  EXPECT_TRUE(HasSuggestion(suggestions, "rect"));
  EXPECT_FALSE(HasSuggestion(suggestions, "circle"));
}

TEST(XmlAutocomplete, SuggestsAttributesInsideOpenTag) {
  constexpr std::string_view kSource = "<svg><rect ";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);

  const std::vector<XmlAutocompleteSuggestion> suggestions =
      BuildXmlAutocompleteSuggestions(context);
  EXPECT_TRUE(HasSuggestion(suggestions, "id"));
  EXPECT_TRUE(HasSuggestion(suggestions, "fill"));
  EXPECT_TRUE(HasSuggestion(suggestions, "transform-origin"));

  const XmlAutocompleteSuggestion* fill = FindSuggestion(suggestions, "fill");
  ASSERT_NE(fill, nullptr);
  EXPECT_TRUE(fill->alsoPresentationAttribute);
}

TEST(XmlAutocomplete, SuggestsStylePropertiesWithColonInsertion) {
  constexpr std::string_view kSource = R"(<svg><rect style="st"/></svg>)";
  const std::size_t cursorOffset = kSource.find("\"st") + 3;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "st");

  const std::vector<XmlAutocompleteSuggestion> suggestions =
      BuildXmlAutocompleteSuggestions(context);
  const XmlAutocompleteSuggestion* stroke = FindSuggestion(suggestions, "stroke");
  ASSERT_NE(stroke, nullptr);
  EXPECT_EQ(stroke->insertText, "stroke: ");
  EXPECT_TRUE(stroke->alsoPresentationAttribute);
  EXPECT_FALSE(HasSuggestion(suggestions, "fill"));
}

TEST(XmlAutocomplete, SuppressesSuggestionsInTextContent) {
  constexpr std::string_view kSource = "<svg>rect";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::TextContent);
  EXPECT_TRUE(BuildXmlAutocompleteSuggestions(context).empty());
}

TEST(XmlAutocomplete, SuggestsStylePropertiesInsideStyleElement) {
  constexpr std::string_view kSource = "<svg><style>rect { fi</style></svg>";
  const std::size_t cursorOffset = kSource.find("fi") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "fi");

  const std::vector<XmlAutocompleteSuggestion> suggestions =
      BuildXmlAutocompleteSuggestions(context);
  EXPECT_TRUE(HasSuggestion(suggestions, "fill"));
  EXPECT_TRUE(HasSuggestion(suggestions, "filter"));
  EXPECT_FALSE(HasSuggestion(suggestions, "stroke"));
}

}  // namespace donner::editor
