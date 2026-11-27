#include "donner/editor/CssSourceReference.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/css/CSS.h"

namespace donner::editor {
namespace {

TEST(CssSourceReferences, RetainsSpellingAndVisitsOnlyTheSelectedFallback) {
  constexpr std::string_view source =
      R"CSS(.test { fill: var(--p\61 int, url("#fallback")) url( #s\70 ectrum ) "url(#text)" /* url(#comment) */ url(other.svg#external); })CSS";
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  const auto sheet = css::CSS::ParseStylesheet(source, warnings);
  ASSERT_THAT(sheet.rules(), testing::SizeIs(1));
  ASSERT_THAT(sheet.rules()[0].declarations, testing::SizeIs(1));
  const auto& components = sheet.rules()[0].declarations[0].values;
  std::vector<std::pair<std::string, std::string>> found;
  const auto collect = [&](const CssSourceReference& ref) {
    const auto begin = *ref.spelling.start.offset;
    found.emplace_back(ref.name, source.substr(begin, *ref.spelling.end.offset - begin));
    return true;
  };
  EXPECT_TRUE(VisitCssSourceReferences(components, source, collect, [](auto) { return true; }));
  using Pair = std::pair<std::string, std::string>;
  EXPECT_THAT(found, testing::ElementsAre(Pair("--paint", R"(--p\61 int)"),
                                          Pair("spectrum", R"(#s\70 ectrum)")));
  found.clear();
  EXPECT_TRUE(VisitCssSourceReferences(components, source, collect, [](auto) { return false; }));
  EXPECT_THAT(found,
              testing::ElementsAre(Pair("--paint", R"(--p\61 int)"), Pair("fallback", "#fallback"),
                                   Pair("spectrum", R"(#s\70 ectrum)")));
}

}  // namespace
}  // namespace donner::editor
