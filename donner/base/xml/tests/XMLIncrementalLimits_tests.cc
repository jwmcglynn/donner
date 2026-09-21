#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/RcString.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLIncrementalParser.h"
#include "donner/base/xml/XMLParser.h"

using testing::HasSubstr;

namespace donner::xml {
namespace {

XMLDocument ParseWithOptions(std::string_view source, const XMLParser::Options& options) {
  ParseResult<XMLDocument> parsed = XMLParser::Parse(source, options);
  EXPECT_FALSE(parsed.hasError());
  return std::move(parsed.result());
}

/// Parse one opening tag under an explicit input ceiling, leaving every other limit at its default.
ParseResult<XMLDocument> ParseOpeningTagWithInputLimit(std::string_view openingTag,
                                                       std::size_t maximumInputSize) {
  XMLParser::Options options;
  options.maximumInputSize = maximumInputSize;
  return XMLIncrementalParser::ParseOpeningTag(openingTag, options);
}

MATCHER_P2(ParsedElementAttributeIs, attributeName, valueMatcher,
           "parses into an element whose attribute '" + std::string(attributeName) + "' " +
               testing::DescribeMatcher<std::optional<RcString>>(valueMatcher, negation)) {
  if (arg.hasError()) {
    *result_listener << "parse failed with " << arg.error();
    return false;
  }

  const std::optional<XMLNode> element = arg.result().root().firstChild();
  if (!element.has_value()) {
    *result_listener << "parsed document has no child node";
    return false;
  }

  return testing::ExplainMatchResult(valueMatcher, element->getAttribute(attributeName),
                                     result_listener);
}

/// Sweep the caller-supplied input ceiling above, at, and below the opening tag's own byte count.
void ExpectOpeningTagInputLimitBoundary(std::string_view openingTag) {
  SCOPED_TRACE(testing::Message() << "opening tag " << openingTag << " (" << openingTag.size()
                                  << " bytes)");
  {
    SCOPED_TRACE("ceiling above the input size");
    EXPECT_THAT(ParseOpeningTagWithInputLimit(openingTag, openingTag.size() + 4),
                ParsedElementAttributeIs("a", testing::Optional(RcString("1"))));
  }
  {
    SCOPED_TRACE("ceiling equal to the input size");
    EXPECT_THAT(ParseOpeningTagWithInputLimit(openingTag, openingTag.size()),
                ParsedElementAttributeIs("a", testing::Optional(RcString("1"))));
  }
  {
    SCOPED_TRACE("ceiling below the input size");
    EXPECT_THAT(ParseOpeningTagWithInputLimit(openingTag, openingTag.size() - 1),
                ParseErrorIs("XML source exceeds maximum input size"));
  }
}

TEST(XMLIncrementalLimits, ParsedAttributeLimitRejectsOpeningTagGrowthTransactionally) {
  XMLParser::Options options;
  options.maxElements = 2;
  options.maxNestingDepth = 2;
  options.maxTotalAttributes = 3;
  XMLDocument document = ParseWithOptions(R"(<root a="1"><child b="2"/></root>)", options);

  std::size_t insertion = document.source().find("/>", document.source().find("<child"));
  ASSERT_NE(insertion, std::string_view::npos);
  ApplySourceEditResult accepted = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(insertion), FileOffset::Offset(insertion)},
      .replacement = R"( c="3")",
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_TRUE(accepted.applied);

  insertion = document.source().find("/>", document.source().find("<child"));
  const std::string sourceBefore(document.source());
  const std::uint64_t versionBefore = document.sourceVersion();
  ApplySourceEditResult rejected = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(insertion), FileOffset::Offset(insertion)},
      .replacement = R"( d="4")",
      .sourceVersion = versionBefore,
  });

  EXPECT_FALSE(rejected.applied);
  ASSERT_TRUE(rejected.diagnostic.has_value());
  EXPECT_THAT(rejected.diagnostic->reason, HasSubstr("total-attribute limit"));
  EXPECT_EQ(document.source(), sourceBefore);
  EXPECT_EQ(document.sourceVersion(), versionBefore);
}

TEST(XMLIncrementalLimits, ParsedNodeLimitRejectsSubtreeGrowthTransactionally) {
  XMLParser::Options options;
  options.maxElements = 4;
  options.maxNestingDepth = 4;
  XMLDocument document = ParseWithOptions(R"(<root><host/></root>)", options);

  const std::string_view initialHost = R"(<host/>)";
  std::size_t editStart = document.source().find(initialHost);
  ASSERT_NE(editStart, std::string_view::npos);
  ApplySourceEditResult accepted = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart),
                           FileOffset::Offset(editStart + initialHost.size())},
      .replacement = R"(<host><child/></host>)",
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_TRUE(accepted.applied);

  const std::size_t insertion = document.source().find("</host>");
  ASSERT_NE(insertion, std::string_view::npos);
  const std::string sourceBefore(document.source());
  const std::uint64_t versionBefore = document.sourceVersion();
  ApplySourceEditResult rejected = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(insertion), FileOffset::Offset(insertion)},
      .replacement = "<extra/><too/>",
      .sourceVersion = versionBefore,
  });

  EXPECT_FALSE(rejected.applied);
  ASSERT_TRUE(rejected.diagnostic.has_value());
  EXPECT_THAT(rejected.diagnostic->reason, HasSubstr("tree-node limit"));
  EXPECT_EQ(document.source(), sourceBefore);
  EXPECT_EQ(document.sourceVersion(), versionBefore);
}

TEST(XMLIncrementalLimits, ParsedDepthLimitRejectsSubtreeGrowthTransactionally) {
  XMLParser::Options options;
  options.maxElements = 8;
  options.maxNestingDepth = 3;
  XMLDocument document = ParseWithOptions(R"(<root><host/></root>)", options);

  const std::string_view initialHost = R"(<host/>)";
  std::size_t editStart = document.source().find(initialHost);
  ASSERT_NE(editStart, std::string_view::npos);
  ApplySourceEditResult accepted = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart),
                           FileOffset::Offset(editStart + initialHost.size())},
      .replacement = R"(<host><level/></host>)",
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_TRUE(accepted.applied);

  const std::string_view level = R"(<level/>)";
  editStart = document.source().find(level);
  ASSERT_NE(editStart, std::string_view::npos);
  const std::string sourceBefore(document.source());
  const std::uint64_t versionBefore = document.sourceVersion();
  ApplySourceEditResult rejected = document.applySourceEdit(XMLEditIntent{
      .range =
          SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editStart + level.size())},
      .replacement = R"(<level><too/></level>)",
      .sourceVersion = versionBefore,
  });

  EXPECT_FALSE(rejected.applied);
  ASSERT_TRUE(rejected.diagnostic.has_value());
  EXPECT_THAT(rejected.diagnostic->reason, HasSubstr("tree-depth limit"));
  EXPECT_EQ(document.source(), sourceBefore);
  EXPECT_EQ(document.sourceVersion(), versionBefore);
}

TEST(XMLIncrementalLimits, ParsedAttributeLimitRejectsSubtreeGrowthTransactionally) {
  XMLParser::Options options;
  options.maxTotalAttributes = 6;
  XMLDocument document =
      ParseWithOptions(R"(<root a="1"><host id="h"><child id="c" x="1"/></host></root>)", options);

  const std::string_view initial = R"(<child id="c" x="1"/>)";
  std::size_t editStart = document.source().find(initial);
  ASSERT_NE(editStart, std::string_view::npos);
  ApplySourceEditResult accepted = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart),
                           FileOffset::Offset(editStart + initial.size())},
      .replacement = R"(<child id="c" x="1"/><new id="n" y="1"/>)",
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_TRUE(accepted.applied);

  const std::string_view current = R"(<child id="c" x="1"/><new id="n" y="1"/>)";
  editStart = document.source().find(current);
  ASSERT_NE(editStart, std::string_view::npos);
  const std::string sourceBefore(document.source());
  const std::uint64_t versionBefore = document.sourceVersion();
  ApplySourceEditResult rejected = document.applySourceEdit(XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(editStart),
                           FileOffset::Offset(editStart + current.size())},
      .replacement = R"(<child id="c" x="1"/><new id="n" y="1"/><extra id="e"/>)",
      .sourceVersion = versionBefore,
  });

  EXPECT_FALSE(rejected.applied);
  ASSERT_TRUE(rejected.diagnostic.has_value());
  EXPECT_THAT(rejected.diagnostic->reason, HasSubstr("total-attribute limit"));
  EXPECT_EQ(document.source(), sourceBefore);
  EXPECT_EQ(document.sourceVersion(), versionBefore);
}

TEST(XMLIncrementalLimits, OpeningTagInputLimitBoundsOrdinaryTag) {
  ExpectOpeningTagInputLimitBoundary(R"(<rect a="1">)");
}

TEST(XMLIncrementalLimits, OpeningTagInputLimitBoundsSelfClosingTag) {
  ExpectOpeningTagInputLimitBoundary(R"(<rect a="1"/>)");
}

TEST(XMLIncrementalLimits, OpeningTagInputLimitRejectsOversizedInput) {
  std::string oversized = R"(<rect a=")";
  oversized.append(64 * 1024, 'x');
  oversized.append(R"(">)");

  EXPECT_THAT(ParseOpeningTagWithInputLimit(oversized, 32),
              ParseErrorIs("XML source exceeds maximum input size"));
}

TEST(XMLIncrementalLimits, OpeningTagInputLimitDoesNotWrapAtMaximumCeiling) {
  EXPECT_THAT(
      ParseOpeningTagWithInputLimit(R"(<rect a="1">)", std::numeric_limits<std::size_t>::max()),
      ParsedElementAttributeIs("a", testing::Optional(RcString("1"))));
}

TEST(XMLIncrementalLimits, OpeningTagInputLimitKeepsParserDiagnosticsForBoundedMalformedTags) {
  EXPECT_THAT(ParseOpeningTagWithInputLimit("<rect", 64),
              ParseErrorIs("Opening tag is missing '>'"));
  EXPECT_THAT(ParseOpeningTagWithInputLimit("<rect a>", 64),
              ParseErrorIs("Attribute name without value, expected '=' followed by a string"));
}

TEST(XMLIncrementalLimits, OpeningTagInputLimitPreservesOtherCallerLimits) {
  XMLParser::Options attributesPerElement;
  attributesPerElement.maximumInputSize = 64;
  attributesPerElement.maxAttributesPerElement = 1;
  EXPECT_THAT(XMLIncrementalParser::ParseOpeningTag(R"(<rect a="1" b="2">)", attributesPerElement),
              ParseErrorIs("Maximum attributes-per-element count exceeded"));

  XMLParser::Options totalAttributes;
  totalAttributes.maximumInputSize = 64;
  totalAttributes.maxTotalAttributes = 1;
  EXPECT_THAT(XMLIncrementalParser::ParseOpeningTag(R"(<rect a="1" b="2">)", totalAttributes),
              ParseErrorIs("Maximum total attribute count exceeded"));
}

}  // namespace
}  // namespace donner::xml
