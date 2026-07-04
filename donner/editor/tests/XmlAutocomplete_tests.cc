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

// --- Cursor / range edge cases ---------------------------------------------

TEST(XmlAutocomplete, CursorPastEndIsClampedToSourceLength) {
  constexpr std::string_view kSource = "<svg><";
  // A cursor beyond the source is clamped to source.size().
  const XmlAutocompleteContext context =
      DetectXmlAutocompleteContext(kSource, kSource.size() + 100);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::ElementName);
  EXPECT_EQ(context.replaceStartOffset, kSource.size());
  EXPECT_EQ(context.replaceEndOffset, kSource.size());
}

TEST(XmlAutocomplete, EmptySourceProducesUnknownContext) {
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext("", 0);
  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::Unknown);
  EXPECT_TRUE(BuildXmlAutocompleteSuggestions(context).empty());
}

// --- Closing tag handling --------------------------------------------------

TEST(XmlAutocomplete, ClosingTagSuppressesAttributeSuggestions) {
  // Cursor inside a `</rect ...` closing tag must not offer attribute names.
  constexpr std::string_view kSource = "<svg><rect/></rect ";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::Unknown);
}

TEST(XmlAutocomplete, AttributeNameStillSuggestedAfterMismatchedCloseTag) {
  // A `</g>` with no matching open `g` exercises the PopElementStack search
  // miss; later autocomplete state must still track the open element correctly.
  constexpr std::string_view kSource = "<svg></g><rect ";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_TRUE(HasSuggestion(BuildXmlAutocompleteSuggestions(context), "fill"));
}

TEST(XmlAutocomplete, AttributeNameStillSuggestedAfterNestedMismatchedCloseTag) {
  // Closing `g` while `rect` is on top exercises the stack search hit path and
  // discards the unmatched inner node.
  constexpr std::string_view kSource = "<svg><g><rect></g><path ";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_TRUE(HasSuggestion(BuildXmlAutocompleteSuggestions(context), "stroke"));
}

TEST(XmlAutocomplete, LeadingClosingTagDoesNotBreakLaterAttributeContext) {
  constexpr std::string_view kSource = "</g><rect ";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_TRUE(HasSuggestion(BuildXmlAutocompleteSuggestions(context), "fill"));
}

TEST(XmlAutocomplete, CommentsCDataAndProcessingInstructionsBeforeCursorAreIgnored) {
  constexpr std::string_view kSource = "<svg><!--note--><![CDATA[raw]]><?target value?><rect ";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_TRUE(HasSuggestion(BuildXmlAutocompleteSuggestions(context), "id"));
}

TEST(XmlAutocomplete, MalformedClosingTagFallsBackToElementNameContext) {
  constexpr std::string_view kSource = "<svg><rect/></rect attr";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::ElementName);
}

TEST(XmlAutocomplete, MalformedOpenTagWithKnownNameFallsBackToAttributeContext) {
  constexpr std::string_view kSource = "<svg><rect @";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
}

TEST(XmlAutocomplete, WhitespaceBeforeTagNameFallsBackToElementName) {
  constexpr std::string_view kSource = "<svg>< ";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::ElementName);
  EXPECT_EQ(context.replaceStartOffset, kSource.size());
  EXPECT_EQ(context.replaceEndOffset, kSource.size());
}

TEST(XmlAutocomplete, CursorOnTagOpenBeforeTokenEndIsUnknown) {
  constexpr std::string_view kSource = "<svg><rect";
  const std::size_t cursorOffset = kSource.find("<rect");
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::Unknown);
}

// --- style="" attribute value edge cases -----------------------------------

TEST(XmlAutocomplete, StyleAttributeAfterColonOffersNoSuggestions) {
  // Once a property name and colon are present, the cursor is in value position
  // and the autocomplete suppresses the property-name list.
  constexpr std::string_view kSource = R"(<svg><rect style="fill: re"/></svg>)";
  const std::size_t cursorOffset = kSource.find("re\"") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::TextContent);
  EXPECT_TRUE(BuildXmlAutocompleteSuggestions(context).empty());
}

TEST(XmlAutocomplete, StyleAttributeSecondPropertyAfterSemicolonResetsSegment) {
  // The segment scanner resets at `;`, so the second declaration gets its own
  // property-name completion.
  constexpr std::string_view kSource = R"(<svg><rect style="fill:red; st"/></svg>)";
  const std::size_t cursorOffset = kSource.find("; st") + 4;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "st");
  EXPECT_TRUE(HasSuggestion(BuildXmlAutocompleteSuggestions(context), "stroke"));
}

TEST(XmlAutocomplete, StyleAttributeSecondPropertyAfterBraceAndWhitespaceResetsSegment) {
  constexpr std::string_view kSource = "<svg><style>rect {\n\tfill: red;\r\n st</style></svg>";
  const std::size_t cursorOffset = kSource.find("st</style>");
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "");
  EXPECT_TRUE(HasSuggestion(BuildXmlAutocompleteSuggestions(context), "stroke"));
}

TEST(XmlAutocomplete, StyleAttributeMidPropertyExtendsReplacementToNameEnd) {
  constexpr std::string_view kSource = R"(<svg><rect style="font-size2: 12px"/></svg>)";
  const std::size_t cursorOffset = kSource.find("font") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "fo");
  EXPECT_EQ(context.replaceStartOffset, kSource.find("font-size2"));
  EXPECT_EQ(context.replaceEndOffset, kSource.find(": 12px"));
}

TEST(XmlAutocomplete, StyleAttributeEmptyValueOffersFullPropertyList) {
  // Cursor right after the opening quote of an empty style value: the segment
  // start equals the cursor, prefix is empty, and the full property list is
  // offered (exercises MakeStylePropertyContext with no prior colon/segment).
  constexpr std::string_view kSource = R"(<svg><rect style=""/></svg>)";
  const std::size_t cursorOffset = kSource.find("\"\"") + 1;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "");
  const std::vector<XmlAutocompleteSuggestion> suggestions =
      BuildXmlAutocompleteSuggestions(context);
  EXPECT_TRUE(HasSuggestion(suggestions, "fill"));
  EXPECT_TRUE(HasSuggestion(suggestions, "stroke"));
}

TEST(XmlAutocomplete, IncompleteQuotedStyleAttributeStaysInAttributeNameRecovery) {
  constexpr std::string_view kSource = R"(<svg><rect style="stro)";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_TRUE(HasSuggestion(BuildXmlAutocompleteSuggestions(context), "style"));
}

TEST(XmlAutocomplete, StylePropertyNameReplacementStopsAfterCssNameCharacters) {
  constexpr std::string_view kSource = "<svg><style>rect { Stroke-2_name: red }</style></svg>";
  const std::size_t cursorOffset = kSource.find("Stroke") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "St");
  EXPECT_EQ(context.replaceStartOffset, kSource.find("Stroke-2"));
  EXPECT_EQ(context.replaceEndOffset, kSource.find("_name"));
}

TEST(XmlAutocomplete, StyleAttributeCursorOnQuotesUsesTokenBoundaryContexts) {
  constexpr std::string_view kSource = R"(<svg><rect style="fill"/></svg>)";

  const XmlAutocompleteContext onOpeningQuote =
      DetectXmlAutocompleteContext(kSource, kSource.find("\"fill"));
  EXPECT_EQ(onOpeningQuote.kind, XmlAutocompleteContextKind::AttributeName);

  const XmlAutocompleteContext onClosingQuote =
      DetectXmlAutocompleteContext(kSource, kSource.find("\"/>"));
  EXPECT_EQ(onClosingQuote.kind, XmlAutocompleteContextKind::StyleValue);
}

TEST(XmlAutocomplete, UnquotedStyleAttributeValueStaysInAttributeNameContext) {
  constexpr std::string_view kSource = "<svg><rect style=fill";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
}

TEST(XmlAutocomplete, NonStyleAttributeValueOffersNoSuggestions) {
  // Attribute value autocomplete is only wired for `style`; a `fill="..."`
  // value must produce no suggestions.
  constexpr std::string_view kSource = R"(<svg><rect fill="re"/></svg>)";
  const std::size_t cursorOffset = kSource.find("re\"") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::Unknown);
  EXPECT_TRUE(BuildXmlAutocompleteSuggestions(context).empty());
}

// --- Element name partial replacement bounds -------------------------------

TEST(XmlAutocomplete, PartialElementNameReportsReplaceSpanAndExtendsToFullName) {
  // Cursor mid-name: the replace span covers the whole existing name and the
  // prefix is only the text up to the cursor.
  constexpr std::string_view kSource = "<svg><rect";
  const std::size_t cursorOffset = kSource.size() - 2;  // between "re" and "ct"
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::ElementName);
  EXPECT_EQ(context.prefix, "re");
  EXPECT_EQ(context.replaceStartOffset, kSource.find("rect"));
  EXPECT_EQ(context.replaceEndOffset, kSource.size());
}

TEST(XmlAutocomplete, PartialAttributeNameAtTokenStartUsesEmptyInsertionContext) {
  constexpr std::string_view kSource = "<svg><rect fill";
  const std::size_t cursorOffset = kSource.find("fill");
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_EQ(context.prefix, "");
  EXPECT_EQ(context.replaceStartOffset, kSource.find("fill"));
  EXPECT_EQ(context.replaceEndOffset, cursorOffset);
}

TEST(XmlAutocomplete, PartialAttributeNameMidTokenReportsFullReplaceSpan) {
  constexpr std::string_view kSource = "<svg><rect fill";
  const std::size_t cursorOffset = kSource.find("fill") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_EQ(context.prefix, "fi");
  EXPECT_EQ(context.replaceStartOffset, kSource.find("fill"));
  EXPECT_EQ(context.replaceEndOffset, kSource.size());
}

TEST(XmlAutocomplete, SuggestionsAreCaseInsensitive) {
  XmlAutocompleteContext context;
  context.kind = XmlAutocompleteContextKind::ElementName;
  context.prefix = "RECT";

  const std::vector<XmlAutocompleteSuggestion> suggestions =
      BuildXmlAutocompleteSuggestions(context);
  EXPECT_TRUE(HasSuggestion(suggestions, "rect"));
  EXPECT_FALSE(HasSuggestion(suggestions, "circle"));
}

TEST(XmlAutocomplete, PrefixLongerThanCandidateSuppressesSuggestion) {
  XmlAutocompleteContext context;
  context.kind = XmlAutocompleteContextKind::ElementName;
  context.prefix = "linearGradientExtra";

  EXPECT_TRUE(BuildXmlAutocompleteSuggestions(context).empty());
}

TEST(XmlAutocomplete, AttributeSuggestionsDeduplicateStructuralPresentationNames) {
  XmlAutocompleteContext context;
  context.kind = XmlAutocompleteContextKind::AttributeName;
  context.prefix = "off";

  const std::vector<XmlAutocompleteSuggestion> suggestions =
      BuildXmlAutocompleteSuggestions(context);
  const auto offsetCount = std::count_if(suggestions.begin(), suggestions.end(),
                                         [](const XmlAutocompleteSuggestion& suggestion) {
                                           return suggestion.displayText == "offset";
                                         });
  EXPECT_EQ(offsetCount, 1);
}

TEST(XmlAutocomplete, AttributeSuggestionsFilterStructuralNamespaceNames) {
  XmlAutocompleteContext context;
  context.kind = XmlAutocompleteContextKind::AttributeName;
  context.prefix = "xlink";

  const std::vector<XmlAutocompleteSuggestion> suggestions =
      BuildXmlAutocompleteSuggestions(context);
  const XmlAutocompleteSuggestion* xlinkHref = FindSuggestion(suggestions, "xlink:href");
  ASSERT_NE(xlinkHref, nullptr);
  EXPECT_EQ(xlinkHref->insertText, "xlink:href");
  EXPECT_FALSE(xlinkHref->alsoPresentationAttribute);
  EXPECT_FALSE(HasSuggestion(suggestions, "href"));
}

}  // namespace donner::editor
