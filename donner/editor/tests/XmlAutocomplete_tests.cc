#include "donner/editor/XmlAutocomplete.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <vector>

#include "donner/editor/CssSourceSyntax.h"

namespace donner::editor {

void PrintTo(const XmlAutocompleteSuggestion& value, std::ostream* out) {
  *out << testing::PrintToString(value.displayText) << " inserts "
       << testing::PrintToString(value.insertText);
}

namespace {

auto HasSuggestionNamed(std::string_view name) {
  return testing::Contains(testing::Field("displayText", &XmlAutocompleteSuggestion::displayText,
                                          testing::Eq(std::string(name))));
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
  EXPECT_THAT(suggestions, HasSuggestionNamed("rect"));
  EXPECT_THAT(suggestions, HasSuggestionNamed("linearGradient"));
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
  EXPECT_THAT(suggestions, HasSuggestionNamed("rect"));
  EXPECT_THAT(suggestions, testing::Not(HasSuggestionNamed("circle")));
}

TEST(XmlAutocomplete, SuggestsAttributesInsideOpenTag) {
  constexpr std::string_view kSource = "<svg><rect ";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);

  const std::vector<XmlAutocompleteSuggestion> suggestions =
      BuildXmlAutocompleteSuggestions(context);
  EXPECT_THAT(suggestions, HasSuggestionNamed("id"));
  EXPECT_THAT(suggestions, HasSuggestionNamed("fill"));
  EXPECT_THAT(suggestions, HasSuggestionNamed("transform-origin"));

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
  EXPECT_THAT(suggestions, testing::Not(HasSuggestionNamed("fill")));
}

TEST(XmlAutocomplete, ReplacesExistingValueAtTheColonAndVariableFunctionBoundaries) {
  constexpr std::string_view source = R"SVG(<svg style="fill:none; stroke:var(--ink)"/>)SVG";
  const auto value = source.find("none");
  const auto context = DetectXmlAutocompleteContext(source, value);
  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssDeclarationValue);
  EXPECT_EQ(context.prefix, "");
  EXPECT_EQ(context.replaceStartOffset, value);
  EXPECT_EQ(context.replaceEndOffset, value + 4);
  const auto custom = source.find("--ink");
  const auto variable = DetectXmlAutocompleteContext(source, custom);
  EXPECT_EQ(variable.kind, XmlAutocompleteContextKind::CssVariableName);
  EXPECT_EQ(variable.prefix, "");
  EXPECT_EQ(variable.replaceStartOffset, custom);
  EXPECT_EQ(variable.replaceEndOffset, custom + 5);
}

TEST(XmlAutocomplete, SuppressesKeywordsAtExistingFunctionalValues) {
  constexpr std::string_view source =
      R"SVG(<svg fill="url(#paint)" style="stroke:var(--ink)"><style>.c { fill:url('#g') }</style></svg>)SVG";
  for (const std::string_view spelling : {"url(#paint)", "var(--ink)", "url('#g')"}) {
    SCOPED_TRACE(spelling);
    const auto start = source.find(spelling);
    for (const auto caret : {start, start + 1}) {
      const auto context = DetectXmlAutocompleteContext(source, caret);
      EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), testing::IsEmpty());
    }
  }
}

TEST(XmlAutocomplete, CompletingAnExistingDeclarationPreservesItsColon) {
  constexpr std::string_view source = R"SVG(<svg style="fill :red;"/>)SVG";
  const auto context = DetectXmlAutocompleteContext(source, source.find("fill") + 2);
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context),
              testing::Contains(testing::Field(&XmlAutocompleteSuggestion::insertText, "fill")));
}

TEST(XmlAutocomplete, SerializesCustomNamesForBothAttributeQuoteStyles) {
  const std::vector<std::string> names = {"--theme:accent", "--quote\"'"};
  XmlAutocompleteContext context{.kind = XmlAutocompleteContextKind::CssVariableName};
  auto suggestions = BuildXmlAutocompleteSuggestions(context, {.customPropertyNames = names});
  EXPECT_THAT(suggestions, testing::Contains(testing::Field(&XmlAutocompleteSuggestion::insertText,
                                                            R"(--theme\:accent)")));
  EXPECT_THAT(suggestions, testing::Contains(testing::Field(&XmlAutocompleteSuggestion::insertText,
                                                            R"(--quote\22 \27 )")));
  context.kind = XmlAutocompleteContextKind::CssDeclarationName;
  suggestions = BuildXmlAutocompleteSuggestions(context, {.customPropertyNames = names});
  EXPECT_THAT(suggestions, testing::Contains(testing::Field(&XmlAutocompleteSuggestion::insertText,
                                                            R"(--theme\:accent: )")));
}

TEST(XmlAutocomplete, EncodedCssPrefixReplacesTheCompleteAuthoredToken) {
  const std::string source = R"(<svg><rect fill="&#110;o" /></svg>)";
  const auto start = source.find("&#110;");
  auto context = DetectXmlAutocompleteContext(source, start + 7);
  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssDeclarationValue);
  EXPECT_EQ(context.propertyName, "fill");
  EXPECT_EQ(context.prefix, "no");
  EXPECT_EQ(context.replaceStartOffset, start);
  EXPECT_EQ(context.replaceEndOffset, start + 7);
  std::vector<std::string_view> keywords = {"none"};
  const auto suggestions = BuildXmlAutocompleteSuggestions(context, {.propertyKeywords = keywords});
  EXPECT_THAT(suggestions, HasSuggestionNamed("none"));
  EXPECT_EQ(DetectXmlAutocompleteContext(source, start + 2).kind,
            XmlAutocompleteContextKind::Unknown);

  const std::string mixed = R"(<svg><rect style="F&#73;LL: no" /></svg>)";
  context = DetectXmlAutocompleteContext(mixed, mixed.find(": no") + 4);
  EXPECT_EQ(context.propertyName, "FILL");
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("none"));
  EXPECT_EQ(context.prefix, "no");
  EXPECT_EQ(mixed.substr(context.replaceStartOffset,
                         context.replaceEndOffset - context.replaceStartOffset),
            "no");
}

TEST(XmlAutocomplete, SelectorCompletionInsertsSerializedCssIdentifiers) {
  const std::vector<std::string> names = {"node:part", "node&part"};
  constexpr std::string_view source = "<svg><style>#node</style></svg>";
  const auto context = DetectXmlAutocompleteContext(source, source.find("#node") + 5);
  const auto suggestions = BuildXmlAutocompleteSuggestions(context, {.ids = names});
  EXPECT_THAT(suggestions,
              testing::Contains(testing::AllOf(
                  testing::Field(&XmlAutocompleteSuggestion::displayText, "#node:part"),
                  testing::Field(&XmlAutocompleteSuggestion::insertText, R"(#node\:part)"))));
  EXPECT_THAT(suggestions, testing::Contains(testing::Field(&XmlAutocompleteSuggestion::insertText,
                                                            R"(#node\26 part)")));
}

TEST(XmlAutocomplete, SuppressesSuggestionsInTextContent) {
  constexpr std::string_view kSource = "<svg>rect";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::TextContent);
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), testing::IsEmpty());
}

TEST(XmlAutocomplete, SuggestsStylePropertiesInsideStyleElement) {
  constexpr std::string_view kSource = "<svg><style>rect { fi</style></svg>";
  const std::size_t cursorOffset = kSource.find("fi") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "fi");

  const std::vector<XmlAutocompleteSuggestion> suggestions =
      BuildXmlAutocompleteSuggestions(context);
  EXPECT_THAT(suggestions, HasSuggestionNamed("fill"));
  EXPECT_THAT(suggestions, HasSuggestionNamed("filter"));
  EXPECT_THAT(suggestions, testing::Not(HasSuggestionNamed("stroke")));
}

TEST(XmlAutocomplete, CompletesSelectorFromSvgAndLiveDocumentNames) {
  constexpr std::string_view kSource = "<svg><style>rect, .bu { fill: red }</style></svg>";
  const std::size_t cursorOffset = kSource.find(".bu") + 3;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssSelector);
  EXPECT_EQ(context.replaceStartOffset, kSource.find(".bu"));
  EXPECT_EQ(context.replaceEndOffset, kSource.find(" {"));
  EXPECT_EQ(context.prefix, ".bu");
  const std::vector<std::string> classes{"button", "card"};
  const std::vector<std::string> ids{"button"};
  const XmlAutocompleteData data{.classNames = classes, .ids = ids};
  const auto suggestions = BuildXmlAutocompleteSuggestions(context, data);
  EXPECT_THAT(suggestions, HasSuggestionNamed(".button"));
  EXPECT_THAT(suggestions, testing::Not(HasSuggestionNamed("#button")));
}

TEST(XmlAutocomplete, ReplacesTheClassNameToTheRightOfTheCaret) {
  const std::string source = "<svg><style>.button {}</style></svg>";
  const auto dot = source.find(".button");
  for (const auto caret : {dot, dot + 1}) {
    const auto context = DetectXmlAutocompleteContext(source, caret);
    EXPECT_EQ(context.prefix, source.substr(dot, caret - dot));
    EXPECT_EQ(context.replaceStartOffset, dot);
    EXPECT_EQ(context.replaceEndOffset, dot + 7);
    std::string edited = source;
    edited.replace(context.replaceStartOffset,
                   context.replaceEndOffset - context.replaceStartOffset, ".card");
    EXPECT_EQ(edited, "<svg><style>.card {}</style></svg>");
  }
}

TEST(XmlAutocomplete, WhitespaceWithinADeclarationListOffersPropertiesAtTheCaret) {
  const std::string source = R"(<svg><rect style="  "/></svg>)";
  const auto caret = source.find("  ") + 1;
  const auto context = DetectXmlAutocompleteContext(source, caret);
  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssDeclarationName);
  EXPECT_EQ(context.prefix, "");
  EXPECT_EQ(context.replaceStartOffset, caret);
  EXPECT_EQ(context.replaceEndOffset, caret);
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("fill"));
}

TEST(XmlAutocomplete, LoneSelectorMarkersRemainInsideTheReplacementRange) {
  const std::vector<std::string> classes{"card"};
  const std::vector<std::string> ids{"target"};
  for (const auto& [marker, expected] :
       std::array<std::pair<std::string_view, std::string_view>, 2>{
           {{".", ".card"}, {"#", "#target"}}}) {
    const std::string source = "<svg><style>" + std::string(marker) + "</style></svg>";
    const std::size_t markerOffset = source.find(marker);
    const XmlAutocompleteContext context =
        DetectXmlAutocompleteContext(source, markerOffset + marker.size());
    SCOPED_TRACE(marker);
    EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssSelector);
    EXPECT_EQ(context.replaceStartOffset, markerOffset);
    EXPECT_EQ(context.replaceEndOffset, markerOffset + marker.size());
    EXPECT_EQ(context.prefix, marker);
    const auto suggestions =
        BuildXmlAutocompleteSuggestions(context, {.classNames = classes, .ids = ids});
    EXPECT_THAT(suggestions, HasSuggestionNamed(expected));
  }
}

TEST(XmlAutocomplete, MixedCaseOrdinaryPropertyUsesCanonicalKeywordMetadata) {
  constexpr std::string_view kSource = R"(<svg><rect style="DiSpLaY:no"/></svg>)";
  const std::size_t cursorOffset = kSource.find("no\"") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssDeclarationValue);
  EXPECT_EQ(context.propertyName, "DiSpLaY");
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("none"));

  constexpr std::string_view kCustom = R"(<svg><rect style="--Tone:no"/></svg>)";
  const std::size_t customCursor = kCustom.find("no\"") + 2;
  const XmlAutocompleteContext custom = DetectXmlAutocompleteContext(kCustom, customCursor);
  EXPECT_EQ(custom.propertyName, "--Tone");
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(custom), testing::IsEmpty());
}

TEST(XmlAutocomplete, CompletesDeclarationValueFromPropertyKeywords) {
  constexpr std::string_view kSource = R"(<svg><rect style="fill: no"/></svg>)";
  const std::size_t cursorOffset = kSource.find("no\"") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssDeclarationValue);
  EXPECT_EQ(context.propertyName, "fill");
  EXPECT_EQ(context.prefix, "no");
  constexpr std::array<std::string_view, 3> keywords{"none", "nonzero", "red"};
  const auto suggestions = BuildXmlAutocompleteSuggestions(context, {.propertyKeywords = keywords});
  EXPECT_THAT(suggestions, HasSuggestionNamed("none"));
  EXPECT_THAT(suggestions, HasSuggestionNamed("nonzero"));
  EXPECT_THAT(suggestions, testing::Not(HasSuggestionNamed("red")));
}

TEST(XmlAutocomplete, CompletesVarFromSourceDeclaredCustomNamesCaseSensitively) {
  constexpr std::string_view kSource = R"(<svg><style>
    :root { --tone: red; --Tone: blue }
    rect { fill: var(--to) }
  </style></svg>)";
  const std::size_t cursorOffset = kSource.find("--to)") + 4;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);
  const std::vector<std::string> names = CollectDeclaredCustomPropertyNames(kSource);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssVariableName);
  EXPECT_EQ(context.prefix, "--to");
  EXPECT_EQ(context.replaceEndOffset, kSource.find(") }"));
  const auto suggestions = BuildXmlAutocompleteSuggestions(context, {.customPropertyNames = names});
  EXPECT_THAT(suggestions, HasSuggestionNamed("--tone"));
  EXPECT_THAT(suggestions, testing::Not(HasSuggestionNamed("--Tone")));
}

TEST(XmlAutocomplete, ReplacesLoneAtSignWithSupportedAtRule) {
  constexpr std::string_view source = "<svg><style>@</style></svg>";
  const auto at = source.find('@');
  const auto context = DetectXmlAutocompleteContext(source, at + 1);
  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssAtRuleName);
  EXPECT_EQ(context.replaceStartOffset, at);
  EXPECT_EQ(context.replaceEndOffset, at + 1);
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("@layer"));
}

TEST(XmlAutocomplete, SuppressesSuggestionsInsideTrailingCssComments) {
  for (const std::string_view source :
       {R"(<svg style="fill: /* comment */"/>)", R"(<svg style="fill: /* comment"/>)",
        "<svg><style>/* comment */</style></svg>"}) {
    SCOPED_TRACE(source);
    const auto caret = source.find("comment") + 3;
    const auto context = DetectXmlAutocompleteContext(source, caret);
    EXPECT_EQ(context.kind, XmlAutocompleteContextKind::Unknown);
    EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), testing::IsEmpty());
  }
}

TEST(XmlAutocomplete, CompletesSupportedAtRules) {
  constexpr std::string_view kSource = "<svg><style>@la</style></svg>";
  const std::size_t cursorOffset = kSource.find("@la") + 3;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssAtRuleName);
  EXPECT_EQ(context.prefix, "@la");
  const auto suggestions = BuildXmlAutocompleteSuggestions(context);
  EXPECT_THAT(suggestions, HasSuggestionNamed("@layer"));
  EXPECT_THAT(suggestions, testing::Not(HasSuggestionNamed("@import")));
}

TEST(XmlAutocomplete, CapsSuggestionsInStableInputOrder) {
  XmlAutocompleteContext context{
      .kind = XmlAutocompleteContextKind::CssSelector,
      .prefix = ".",
  };
  std::vector<std::string> classes;
  for (int i = 0; i < 300; ++i) {
    char name[8];
    std::snprintf(name, sizeof(name), "c%03d", i);
    classes.emplace_back(name);
  }
  const auto suggestions = BuildXmlAutocompleteSuggestions(context, {.classNames = classes});
  std::vector<testing::Matcher<const XmlAutocompleteSuggestion&>> expected;
  for (std::size_t i = 0; i < 256; ++i) {
    const std::string spelling = "." + classes[i];
    expected.push_back(testing::AllOf(
        testing::Field("displayText", &XmlAutocompleteSuggestion::displayText, spelling),
        testing::Field("insertText", &XmlAutocompleteSuggestion::insertText, spelling)));
  }
  EXPECT_THAT(suggestions, testing::ElementsAreArray(expected));
}

TEST(XmlAutocomplete, SelectorCatalogStreamsDeduplicatesAndSharesLimits) {
  XmlAutocompleteSelectorCatalog catalog({.values = 4, .bytes = 20});
  catalog.addClassList("beta  alpha\tbeta");
  catalog.addId("node");
  catalog.addId("node");
  catalog.addClassList("gamma trailing");
  catalog.finish();

  EXPECT_TRUE(catalog.full());
  EXPECT_THAT(catalog.classNames(), testing::ElementsAre("alpha", "beta", "gamma"));
  EXPECT_THAT(catalog.ids(), testing::ElementsAre("node"));
}

TEST(XmlAutocomplete, SelectorCatalogStopsBeforeExceedingSharedByteBudget) {
  XmlAutocompleteSelectorCatalog catalog({.values = 10, .bytes = 9});
  catalog.addClassList("one two three");
  catalog.addId("id");
  catalog.finish();

  EXPECT_TRUE(catalog.full());
  EXPECT_THAT(catalog.classNames(), testing::ElementsAre("one", "two"));
  EXPECT_THAT(catalog.ids(), testing::IsEmpty());
}

TEST(XmlAutocomplete, SelectorInspectionCountsRepeatedNamesAcrossCalls) {
  XmlAutocompleteSelectorCatalog catalog({.scanTokens = 3});
  catalog.addClassList("one one");
  catalog.finish();
  catalog.addClassList("one");
  catalog.addId("outside");
  catalog.finish();
  EXPECT_TRUE(catalog.full());
  EXPECT_THAT(catalog.classNames(), testing::ElementsAre("one"));
  EXPECT_THAT(catalog.ids(), testing::IsEmpty());

  XmlAutocompleteSelectorCatalog prefix({.scanBytes = 6});
  prefix.addClassList("one three");
  prefix.finish();
  EXPECT_THAT(prefix.classNames(), testing::ElementsAre("one"));
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
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), testing::IsEmpty());
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
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("fill"));
}

TEST(XmlAutocomplete, AttributeNameStillSuggestedAfterNestedMismatchedCloseTag) {
  // Closing `g` while `rect` is on top exercises the stack search hit path and
  // discards the unmatched inner node.
  constexpr std::string_view kSource = "<svg><g><rect></g><path ";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("stroke"));
}

TEST(XmlAutocomplete, LeadingClosingTagDoesNotBreakLaterAttributeContext) {
  constexpr std::string_view kSource = "</g><rect ";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("fill"));
}

TEST(XmlAutocomplete, CommentsCDataAndProcessingInstructionsBeforeCursorAreIgnored) {
  constexpr std::string_view kSource = "<svg><!--note--><![CDATA[raw]]><?target value?><rect ";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("id"));
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

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssDeclarationValue);
  EXPECT_EQ(context.propertyName, "fill");
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), testing::IsEmpty());
}

TEST(XmlAutocomplete, StyleAttributeSecondPropertyAfterSemicolonResetsSegment) {
  // The segment scanner resets at `;`, so the second declaration gets its own
  // property-name completion.
  constexpr std::string_view kSource = R"(<svg><rect style="fill:red; st"/></svg>)";
  const std::size_t cursorOffset = kSource.find("; st") + 4;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "st");
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("stroke"));
}

TEST(XmlAutocomplete, SingleQuotedStyleAttributeSuggestsProperties) {
  constexpr std::string_view kSource = "<svg><rect style='Fi'/></svg>";
  const std::size_t cursorOffset = kSource.find("Fi") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "Fi");
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("fill"));
}

TEST(XmlAutocomplete, StyleAttributeSecondPropertyAfterBraceAndWhitespaceResetsSegment) {
  constexpr std::string_view kSource = "<svg><style>rect {\n\tfill: red;\r\n st</style></svg>";
  const std::size_t cursorOffset = kSource.find("st</style>");
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "");
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("stroke"));
}

TEST(XmlAutocomplete, StyleElementSkipsTabBeforePropertyName) {
  constexpr std::string_view kSource = "<svg><style>rect {\tst</style></svg>";
  const std::size_t cursorOffset = kSource.find("st</style>") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "st");
  EXPECT_EQ(context.replaceStartOffset, kSource.find("st</style>"));
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("stroke"));
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
  EXPECT_THAT(suggestions, HasSuggestionNamed("fill"));
  EXPECT_THAT(suggestions, HasSuggestionNamed("stroke"));
}

TEST(XmlAutocomplete, IncompleteQuotedStyleAttributeStaysInAttributeNameRecovery) {
  constexpr std::string_view kSource = R"(<svg><rect style="stro)";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("style"));
}

TEST(XmlAutocomplete, StylePropertyNameReplacementStopsAfterCssNameCharacters) {
  constexpr std::string_view kSource = "<svg><style>rect { Stroke-2_name: red }</style></svg>";
  const std::size_t cursorOffset = kSource.find("Stroke") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "St");
  EXPECT_EQ(context.replaceStartOffset, kSource.find("Stroke-2"));
  EXPECT_EQ(context.replaceEndOffset, kSource.find(": red"));
}

TEST(XmlAutocomplete, StylePropertyReplacementAcceptsAllCssNameCharacterClasses) {
  constexpr std::string_view kSource = "<svg><style>a { Ab9-c: red }</style></svg>";
  const std::size_t cursorOffset = kSource.find("Ab9") + 1;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::StyleValue);
  EXPECT_EQ(context.prefix, "A");
  EXPECT_EQ(context.replaceStartOffset, kSource.find("Ab9-c"));
  EXPECT_EQ(context.replaceEndOffset, kSource.find(": red"));
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

TEST(XmlAutocomplete, CompleteUnquotedStyleAttributeValueStaysInAttributeNameRecovery) {
  constexpr std::string_view kSource = "<svg><rect style=fill/>";
  const std::size_t cursorOffset = kSource.find("fill") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_EQ(context.prefix, "fi");
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("filter"));
}

TEST(XmlAutocomplete, CompleteQuotedAttributeBeforeCursorClearsAttributeNameState) {
  constexpr std::string_view kSource = R"(<svg><rect id="target" )";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_EQ(context.prefix, "");
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("fill"));
}

TEST(XmlAutocomplete, CursorAfterQuotedStyleAttributeValueIsUnknown) {
  constexpr std::string_view kSource = R"(<svg><rect style="fill"/></svg>)";
  const std::size_t cursorOffset = kSource.find("\"/>") + 1;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::Unknown);
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), testing::IsEmpty());
}

TEST(XmlAutocomplete, PresentationAttributeValueUsesCssValueContext) {
  constexpr std::string_view kSource = R"(<svg><rect fill="re"/></svg>)";
  const std::size_t cursorOffset = kSource.find("re\"") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::CssDeclarationValue);
  EXPECT_EQ(context.propertyName, "fill");
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), testing::IsEmpty());
}

TEST(XmlAutocomplete, CursorInsideMalformedClosingTagRecoversAsElementName) {
  constexpr std::string_view kSource = "<svg><rect></rect bogus";
  const std::size_t cursorOffset = kSource.find("bogus") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::ElementName);
  EXPECT_EQ(context.prefix, "");
  EXPECT_EQ(context.replaceStartOffset, cursorOffset);
  EXPECT_EQ(context.replaceEndOffset, cursorOffset);
}

TEST(XmlAutocomplete, ClosedStyleElementReturnsToTextContentContext) {
  constexpr std::string_view kSource = "<svg><style>rect { fill:red }</style>fi</svg>";
  const std::size_t cursorOffset = kSource.find("fi</svg>") + 2;
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, cursorOffset);

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::TextContent);
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), testing::IsEmpty());
}

TEST(XmlAutocomplete, DeclarationDoctypeAndEntityBeforeCursorAreIgnored) {
  constexpr std::string_view kSource = R"(<?xml version="1.0"?><!DOCTYPE svg><svg>&amp;<rect )";
  const XmlAutocompleteContext context = DetectXmlAutocompleteContext(kSource, kSource.size());

  EXPECT_EQ(context.kind, XmlAutocompleteContextKind::AttributeName);
  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), HasSuggestionNamed("class"));
}

TEST(XmlAutocomplete, CursorOnSelfCloseAndCommentTokensIsUnknown) {
  constexpr std::string_view kSelfClose = "<svg><rect/>";
  const XmlAutocompleteContext selfClose =
      DetectXmlAutocompleteContext(kSelfClose, kSelfClose.size());
  EXPECT_EQ(selfClose.kind, XmlAutocompleteContextKind::Unknown);

  constexpr std::string_view kComment = "<svg><!--note--></svg>";
  const XmlAutocompleteContext comment =
      DetectXmlAutocompleteContext(kComment, kComment.find("note"));
  EXPECT_EQ(comment.kind, XmlAutocompleteContextKind::Unknown);
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
  EXPECT_THAT(suggestions, HasSuggestionNamed("rect"));
  EXPECT_THAT(suggestions, testing::Not(HasSuggestionNamed("circle")));
}

TEST(XmlAutocomplete, PrefixLongerThanCandidateSuppressesSuggestion) {
  XmlAutocompleteContext context;
  context.kind = XmlAutocompleteContextKind::ElementName;
  context.prefix = "linearGradientExtra";

  EXPECT_THAT(BuildXmlAutocompleteSuggestions(context), testing::IsEmpty());
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
  EXPECT_THAT(suggestions, testing::Not(HasSuggestionNamed("href")));
}

}  // namespace donner::editor
