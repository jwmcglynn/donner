#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLParser.h"

using testing::HasSubstr;

namespace donner::xml {
namespace {

XMLDocument ParseWithOptions(std::string_view source, const XMLParser::Options& options) {
  ParseResult<XMLDocument> parsed = XMLParser::Parse(source, options);
  EXPECT_FALSE(parsed.hasError());
  return std::move(parsed.result());
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

}  // namespace
}  // namespace donner::xml
