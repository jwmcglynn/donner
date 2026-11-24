/// @file
/// A frame that restyles the whole document, as every class, `style`, presentation-attribute or
/// stylesheet edit does, must compute every style exactly as the document's first frame does. This
/// runs that comparison over every document of the repository's SVG corpora.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/renderer/tests/ComputedStyleSnapshot.h"
#include "donner/svg/renderer/tests/SvgCorpus.h"

namespace donner::svg::components {
namespace {

using tests::CorpusDocument;
using tests::CorpusDocuments;

class FullRestyleCorpusTest : public ::testing::TestWithParam<CorpusDocument> {};

TEST_P(FullRestyleCorpusTest, FullRestyleMatchesFirstFrame) {
  ASSERT_THAT(std::filesystem::file_size(GetParam().path), ::testing::Gt(0u)) << GetParam();
  std::optional<SVGDocument> parsed = tests::ParseCorpusDocument(GetParam());
  if (!parsed) {
    GTEST_SKIP() << "Does not parse";
  }
  SVGDocument& document = *parsed;
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  Registry& registry = document.registry();
  auto& context = registry.ctx().emplace<RenderingContext>(registry);

  context.instantiateRenderTree(false, warnings);
  const std::vector<PositionedStyle> firstFrame = PositionedStyles(document);

  // What an edit that restyles the whole document requests.
  context.invalidateRenderTree();
  context.instantiateRenderTree(false, warnings);

  EXPECT_THAT(PositionedStyles(document), ::testing::ElementsAreArray(firstFrame));
}

INSTANTIATE_TEST_SUITE_P(Corpus, FullRestyleCorpusTest, ::testing::ValuesIn(CorpusDocuments()),
                         [](const ::testing::TestParamInfo<CorpusDocument>& info) {
                           return info.param.name;
                         });

}  // namespace
}  // namespace donner::svg::components
