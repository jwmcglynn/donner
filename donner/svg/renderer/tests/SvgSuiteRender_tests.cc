#include "donner/svg/renderer/tests/SvgSuiteRender.h"

#include <array>
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
  std::filesystem::path stagedInput() const { return root_ / "sandbox/tests/deep/input.svg"; }
  std::filesystem::path resourceRoot() const { return root_ / "sandbox/resources"; }

  void writeStagedInput(std::string_view href) {
    std::error_code error;
    std::filesystem::create_directories(stagedInput().parent_path(), error);
    ASSERT_FALSE(error) << error;
    std::ofstream(stagedInput()) << "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'>"
                                    "<image width='1' height='1' href='"
                                 << href << "'/></svg>";
  }

  void writeRedResource(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    ASSERT_FALSE(error) << error;
    const std::array<uint8_t, 4> red{255, 0, 0, 255};
    ASSERT_THAT(svg_suite::WritePng(path, {1, 1, 1, red}), testing::IsEmpty());
  }

  svg_suite::ImageResult renderStagedInput() {
    return RenderSvgSuiteCase({stagedInput(), resourceRoot(), {}, 1, 1, root_ / "sandbox"});
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

TEST_F(SvgSuiteRenderTest, StagedInputUsesItsDocumentDirectoryForRelativeResources) {
  writeRedResource(resourceRoot() / "sub/red.png");
  writeStagedInput("../../resources/sub/red.png");
  const auto result = renderStagedInput();
  ASSERT_THAT(result.error, testing::IsEmpty());
  ASSERT_NE(result.image, std::nullopt);
  EXPECT_THAT(result.image->data, testing::ElementsAre(255, 0, 0, 255));
}

TEST_F(SvgSuiteRenderTest, InRootInputKeepsDocumentRelativeResources) {
  writeRedResource(resourceRoot() / "sub/red.png");
  const auto input = resourceRoot() / "input.svg";
  std::ofstream(input) << "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'>"
                          "<image width='1' height='1' href='sub/red.png'/></svg>";
  const auto result = RenderSvgSuiteCase({input, resourceRoot(), {}, 1, 1});
  ASSERT_THAT(result.error, testing::IsEmpty());
  ASSERT_NE(result.image, std::nullopt);
  EXPECT_THAT(result.image->data, testing::ElementsAre(255, 0, 0, 255));
}

TEST_F(SvgSuiteRenderTest, StagedInputCannotReadOutsideResourceRoot) {
  writeRedResource(resourceRoot() / "sub/red.png");
  writeRedResource(root_ / "sandbox/tests/secret.png");
  writeRedResource(root_ / "sandbox/resources-sibling/secret.png");
  writeRedResource(root_ / "outside/secret.png");
  for (const std::string_view href :
       {"../secret.png", "../../resources-sibling/secret.png", "../../../outside/secret.png"}) {
    SCOPED_TRACE(href);
    writeStagedInput(href);
    const auto result = renderStagedInput();
    ASSERT_THAT(result.error, testing::IsEmpty());
    ASSERT_NE(result.image, std::nullopt);
    EXPECT_THAT(result.image->data, testing::ElementsAre(0, 0, 0, 0));
  }
}

TEST_F(SvgSuiteRenderTest, StagedInputCannotFollowResourceSymlinkOutsideRoot) {
  writeRedResource(resourceRoot() / "sub/red.png");
  writeRedResource(root_ / "outside/secret.png");
  std::error_code error;
  std::filesystem::create_symlink(root_ / "outside/secret.png", resourceRoot() / "sub/link.png",
                                  error);
  ASSERT_FALSE(error) << error;
  writeStagedInput("../../resources/sub/link.png");
  const auto result = renderStagedInput();
  ASSERT_THAT(result.error, testing::IsEmpty());
  ASSERT_NE(result.image, std::nullopt);
  EXPECT_THAT(result.image->data, testing::ElementsAre(0, 0, 0, 0));
}

TEST_F(SvgSuiteRenderTest, OversizedOutputFails) {
  EXPECT_THAT(RenderSvgSuiteCase({root_ / "input.svg", root_, {}, 16384, 16384}).error,
              testing::HasSubstr("unsupported render dimensions"));
}

}  // namespace
}  // namespace donner::svg
