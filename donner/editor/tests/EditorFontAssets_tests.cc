#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <stb/stb_truetype.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/editor/EditorSymbolGlyphs.h"
#include "embed_resources/FiraCodeFont.h"
#include "embed_resources/RobotoFont.h"

namespace donner::editor {
namespace {

using testing::IsEmpty;
using testing::Not;

struct EmbeddedFontCase {
  std::string_view name;
  std::span<const unsigned char> embeddedBytes;
  std::string_view upstreamRunfile;
  std::size_t completeUpstreamSize;
};

std::vector<unsigned char> ReadRunfile(std::string_view path) {
  std::ifstream input(Runfiles::instance().Rlocation(std::string(path)), std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(EditorFontAssetsTest, EmbeddedUiFontsPreserveCompleteUpstreamAssets) {
  const std::array fonts = {
      EmbeddedFontCase{"Roboto Regular", embedded::kRobotoRegularTtf,
                       "third_party/roboto/Roboto-Regular.ttf", 168260u},
      EmbeddedFontCase{"Roboto Bold", embedded::kRobotoBoldTtf,
                       "third_party/roboto/Roboto-Bold.ttf", 167336u},
      EmbeddedFontCase{"Fira Code Regular", embedded::kFiraCodeRegularTtf,
                       "third_party/fira-code/FiraCode-Regular.ttf", 289624u},
  };
  for (const EmbeddedFontCase& font : fonts) {
    const std::vector<unsigned char> upstreamBytes = ReadRunfile(font.upstreamRunfile);
    ASSERT_THAT(upstreamBytes, Not(IsEmpty())) << font.name << " upstream font could not be read";
    ASSERT_EQ(upstreamBytes.size(), font.completeUpstreamSize)
        << font.name << " tracked asset must remain the complete upstream font, not a subset";
    ASSERT_EQ(font.embeddedBytes.size(), font.completeUpstreamSize)
        << font.name << " embedded asset must retain the complete upstream glyph inventory";
    ASSERT_EQ(font.embeddedBytes.size(), upstreamBytes.size())
        << font.name << " must retain the complete upstream glyph inventory";

    const auto mismatch =
        std::mismatch(font.embeddedBytes.begin(), font.embeddedBytes.end(), upstreamBytes.begin());
    const std::size_t mismatchOffset =
        static_cast<std::size_t>(std::distance(font.embeddedBytes.begin(), mismatch.first));
    EXPECT_EQ(mismatchOffset, upstreamBytes.size())
        << font.name << " first differs from the upstream asset at byte " << mismatchOffset;
  }
}

// Every non-ASCII glyph the editor chrome draws has to exist in the fonts the
// editor ships. ImGui builds its atlas from the requested ranges intersected
// with the font's `cmap`: a codepoint the font does not have is dropped
// silently and every draw of it falls back to the font's fallback character, a
// literal `?`. That is how the source pane's unnumbered reference chips ended
// up rendering `?` - they asked Roboto for dingbats it has never contained.
TEST(EditorFontAssetsTest, EmbeddedFontsProvideEveryEditorSymbolGlyph) {
  const std::array fonts = {
      std::pair<std::string_view, std::span<const unsigned char>>{"Roboto Regular",
                                                                  embedded::kRobotoRegularTtf},
      std::pair<std::string_view, std::span<const unsigned char>>{"Roboto Bold",
                                                                  embedded::kRobotoBoldTtf},
      std::pair<std::string_view, std::span<const unsigned char>>{"Fira Code Regular",
                                                                  embedded::kFiraCodeRegularTtf},
  };

  for (const auto& [name, bytes] : fonts) {
    stbtt_fontinfo font = {};
    ASSERT_NE(stbtt_InitFont(&font, bytes.data(), stbtt_GetFontOffsetForIndex(bytes.data(), 0)), 0)
        << name << " could not be parsed";
    for (const char32_t codepoint : kEditorSymbolCodepoints) {
      EXPECT_NE(stbtt_FindGlyphIndex(&font, static_cast<int>(codepoint)), 0)
          << name << " has no glyph for U+" << std::hex << std::uppercase
          << static_cast<std::uint32_t>(codepoint)
          << ", so the editor chrome draws its fallback '?' instead";
    }
  }
}

}  // namespace
}  // namespace donner::editor
