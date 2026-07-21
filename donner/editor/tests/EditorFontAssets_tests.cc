#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/tests/Runfiles.h"
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

}  // namespace
}  // namespace donner::editor
