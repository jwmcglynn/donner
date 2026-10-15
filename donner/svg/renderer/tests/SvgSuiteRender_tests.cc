#include "donner/svg/renderer/tests/SvgSuiteRender.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

class SvgSuiteRenderTest : public testing::Test {
protected:
  void SetUp() override {
    const char* temporary = std::getenv("TEST_TMPDIR");
    ASSERT_NE(temporary, nullptr);
    std::string name = (std::filesystem::path(temporary) / "suite-render-XXXXXX").string();
    ASSERT_NE(mkdtemp(name.data()), nullptr);
    root_ = name;
    std::ofstream(root_ / "input.svg")
        << R"(<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"><rect width="1" height="1" fill="red"/></svg>)";
  }
  std::filesystem::path root_;
};

TEST_F(SvgSuiteRenderTest, RendersThroughSoftwareBackend) {
  const auto result = RenderSvgSuiteCase({root_ / "input.svg", root_, {}, 1, 1});
  ASSERT_THAT(result.error, testing::IsEmpty());
  ASSERT_NE(result.image, std::nullopt);
  EXPECT_THAT(result.image->data, testing::ElementsAre(255, 0, 0, 255));
}

TEST_F(SvgSuiteRenderTest, MissingRequiredFontDirectoryFails) {
  const auto result = RenderSvgSuiteCase({root_ / "input.svg", root_, root_ / "missing", 1, 1});
  EXPECT_THAT(result.error, testing::HasSubstr("font root"));
  EXPECT_EQ(result.image, std::nullopt);
}

TEST_F(SvgSuiteRenderTest, InvalidRequiredFontFails) {
  std::error_code error;
  std::filesystem::create_directory(root_ / "fonts", error);
  ASSERT_FALSE(error) << error;
  std::ofstream(root_ / "fonts" / "invalid.ttf") << "not a font";
  const auto result = RenderSvgSuiteCase({root_ / "input.svg", root_, root_ / "fonts", 1, 1});
  EXPECT_THAT(result.error, testing::HasSubstr("font metadata is invalid"));
  EXPECT_EQ(result.image, std::nullopt);
}

TEST_F(SvgSuiteRenderTest, InputOutsideRootFails) {
  std::error_code error;
  std::filesystem::create_directory(root_ / "other", error);
  ASSERT_FALSE(error) << error;
  const auto result = RenderSvgSuiteCase({root_ / "input.svg", root_ / "other", {}, 1, 1});
  EXPECT_THAT(result.error, testing::HasSubstr("escapes resource root"));
}

TEST_F(SvgSuiteRenderTest, ExplicitStagedInputRootDoesNotExpandResourceRoot) {
  std::error_code error;
  std::filesystem::create_directory(root_ / "resources", error);
  ASSERT_FALSE(error) << error;
  const auto result =
      RenderSvgSuiteCase({root_ / "input.svg", root_ / "resources", {}, 1, 1, root_});
  ASSERT_THAT(result.error, testing::IsEmpty());
  ASSERT_NE(result.image, std::nullopt);
  EXPECT_THAT(result.image->data, testing::ElementsAre(255, 0, 0, 255));
  EXPECT_THAT(
      RenderSvgSuiteCase({root_ / "input.svg", root_ / "resources", {}, 1, 1, root_ / "resources"})
          .error,
      testing::HasSubstr("escapes resource root"));
}

TEST_F(SvgSuiteRenderTest, OversizedOutputFails) {
  EXPECT_THAT(RenderSvgSuiteCase({root_ / "input.svg", root_, {}, 16384, 16384}).error,
              testing::HasSubstr("unsupported render dimensions"));
}

}  // namespace
}  // namespace donner::svg
