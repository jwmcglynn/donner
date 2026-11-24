/// @file
/// A frame that restyles the whole document, as every class, `style`, presentation-attribute or
/// stylesheet edit does, must compute every style exactly as the document's first frame does. This
/// runs that comparison over every document of the repository's SVG corpora.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/renderer/tests/ComputedStyleSnapshot.h"

namespace donner::svg::components {
namespace {

/// A corpus document: its path and a test name derived from where it sits in the corpus.
struct CorpusDocument {
  std::filesystem::path path;
  std::string name;
};

std::ostream& operator<<(std::ostream& os, const CorpusDocument& document) {
  return os << document.path.string();
}

void AddDocuments(const std::filesystem::path& root, std::string_view prefix,
                  std::vector<CorpusDocument>& documents) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (entry.path().extension() != ".svg") {
      continue;
    }
    std::string name(prefix);
    for (const char c : entry.path().lexically_relative(root).replace_extension().string()) {
      name.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    documents.push_back({entry.path(), std::move(name)});
  }
}

std::vector<CorpusDocument> CorpusDocuments() {
  std::vector<CorpusDocument> documents;
  AddDocuments(Runfiles::instance().Rlocation("third_party/resvg-test-suite/tests"), "resvg_",
               documents);
  AddDocuments(Runfiles::instance().Rlocation("donner/svg/renderer/testdata"), "testdata_",
               documents);
  documents.push_back({Runfiles::instance().Rlocation("donner_splash.svg"), "donner_splash"});
  documents.push_back(
      {Runfiles::instance().Rlocation("third_party/bootstrap-icons/bootstrap-icons.svg"),
       "bootstrap_icons"});
  std::sort(
      documents.begin(), documents.end(),
      [](const CorpusDocument& lhs, const CorpusDocument& rhs) { return lhs.name < rhs.name; });
  return documents;
}

class FullRestyleCorpusTest : public ::testing::TestWithParam<CorpusDocument> {};

TEST_P(FullRestyleCorpusTest, FullRestyleMatchesFirstFrame) {
  std::ifstream file(GetParam().path, std::ios::binary);
  std::ostringstream source;
  source << file.rdbuf();
  ASSERT_THAT(source.str(), ::testing::Not(::testing::IsEmpty())) << "Unreadable: " << GetParam();

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source.str(), warnings);
  if (parsed.hasError()) {
    GTEST_SKIP() << "Does not parse: " << parsed.error();
  }
  SVGDocument document = std::move(parsed.result());
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
