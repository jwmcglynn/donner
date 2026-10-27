#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include "donner/base/PackagedResource.h"
#include "donner/base/tests/Runfiles.h"
#include "embed_resources/ImGuiDefaultFont.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "imgui.h"

namespace donner {
namespace {

class ImGuiDefaultFontProviderTest : public testing::Test {
protected:
  void SetUp() override { original_ = GetPackagedResourceSession(); }
  void TearDown() override { EXPECT_TRUE(SetPackagedResourceSession(original_)); }

  std::shared_ptr<PackagedResourceSession> original_;
};

TEST_F(ImGuiDefaultFontProviderTest, AtlasOwnsAnIndependentCopyAfterResourceSessionTeardown) {
  std::ifstream input(Runfiles::instance().Rlocation("third_party/imgui/proggy_fixture.ttf"),
                      std::ios::binary);
  ResourceBytes source = std::make_shared<const std::vector<uint8_t>>(
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  ASSERT_EQ(source->size(), 41208u);
  auto session = std::make_shared<PackagedResourceSession>(
      [source](const PackagedResource&) { return source; });
  ASSERT_TRUE(SetPackagedResourceSession(session));
  ImFontAtlas atlas;
  ImFontConfig config;
  config.FontDataOwnedByAtlas = false;
  ASSERT_NE(atlas.AddFontDefault(&config), nullptr);
  ASSERT_EQ(atlas.ConfigData.Size, 1);
  EXPECT_TRUE(atlas.ConfigData[0].FontDataOwnedByAtlas);
  EXPECT_NE(atlas.ConfigData[0].FontData, source->data());
  EXPECT_EQ(atlas.ConfigData[0].FontDataSize, static_cast<int>(source->size()));
  EXPECT_THAT(
      std::vector<uint8_t>(static_cast<uint8_t*>(atlas.ConfigData[0].FontData),
                           static_cast<uint8_t*>(atlas.ConfigData[0].FontData) + source->size()),
      testing::ElementsAreArray(*source));
  ASSERT_TRUE(SetPackagedResourceSession(std::make_shared<PackagedResourceSession>(
      [](const PackagedResource&) -> ResourceBytes { return {}; })));
  source.reset();
  session.reset();
  ASSERT_TRUE(atlas.Build());
  ASSERT_EQ(atlas.Fonts.Size, 1);
  EXPECT_NE(atlas.Fonts[0]->FindGlyphNoFallback('A'), nullptr);
}

TEST_F(ImGuiDefaultFontProviderTest, MissingAssetFailsClearlyWithoutAnEmbeddedFallback) {
  ASSERT_TRUE(SetPackagedResourceSession(std::make_shared<PackagedResourceSession>(
      [](const PackagedResource&) -> ResourceBytes { return {}; })));
  ImFontAtlas atlas;
  testing::internal::CaptureStderr();
  EXPECT_EQ(atlas.AddFontDefault(), nullptr);
  EXPECT_FALSE(atlas.Build());
  const std::string error = testing::internal::GetCapturedStderr();
  EXPECT_THAT(error, testing::HasSubstr("ImGui default font resource unavailable"));
  EXPECT_THAT(GetPackagedResourceSession()->error(), testing::HasSubstr("kProggyCleanTtf"));
  EXPECT_EQ(atlas.ConfigData.Size, 0);
  EXPECT_EQ(atlas.Fonts.Size, 0);
}

}  // namespace
}  // namespace donner
