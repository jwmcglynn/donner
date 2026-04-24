#include "donner/editor/LocalPathDisplay.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace donner::editor {
namespace {

const std::filesystem::path kCwd = "/workspace/donner";

TEST(LocalPathDisplayTest, EmptyPassesThrough) {
  EXPECT_EQ(PrettifyLocalPath("", kCwd), "");
}

TEST(LocalPathDisplayTest, HttpsPassesThrough) {
  EXPECT_EQ(PrettifyLocalPath("https://example.com/x.svg", kCwd), "https://example.com/x.svg");
}

TEST(LocalPathDisplayTest, HttpPassesThrough) {
  EXPECT_EQ(PrettifyLocalPath("http://example.com/x.svg", kCwd), "http://example.com/x.svg");
}

TEST(LocalPathDisplayTest, DataUriPassesThrough) {
  EXPECT_EQ(PrettifyLocalPath("data:image/svg+xml,<svg/>", kCwd), "data:image/svg+xml,<svg/>");
}

TEST(LocalPathDisplayTest, AbsolutePathInsideCwdBecomesRelative) {
  EXPECT_EQ(PrettifyLocalPath("/workspace/donner/donner_splash.svg", kCwd), "./donner_splash.svg");
}

TEST(LocalPathDisplayTest, AbsolutePathInSubdirGetsSubdirRelative) {
  EXPECT_EQ(PrettifyLocalPath("/workspace/donner/testdata/foo.svg", kCwd), "./testdata/foo.svg");
}

TEST(LocalPathDisplayTest, FileUriInsideCwdIsRelativized) {
  EXPECT_EQ(PrettifyLocalPath("file:///workspace/donner/donner_splash.svg", kCwd),
            "./donner_splash.svg");
}

TEST(LocalPathDisplayTest, AbsolutePathOutsideCwdPassesThrough) {
  EXPECT_EQ(PrettifyLocalPath("/other/foo.svg", kCwd), "/other/foo.svg");
}

TEST(LocalPathDisplayTest, FileUriOutsideCwdPassesThrough) {
  EXPECT_EQ(PrettifyLocalPath("file:///other/foo.svg", kCwd), "file:///other/foo.svg");
}

TEST(LocalPathDisplayTest, BareRelativePathGetsDotSlash) {
  EXPECT_EQ(PrettifyLocalPath("donner_splash.svg", kCwd), "./donner_splash.svg");
}

TEST(LocalPathDisplayTest, AlreadyPrefixedRelativePathStays) {
  // `./foo.svg` lexically normalizes to `/workspace/donner/foo.svg`
  // against baseDir; `lexically_relative` against cwd gives `foo.svg`;
  // we re-prefix with `./` → `./foo.svg`. Idempotent.
  EXPECT_EQ(PrettifyLocalPath("./donner_splash.svg", kCwd), "./donner_splash.svg");
}

TEST(LocalPathDisplayTest, ParentDirectoryPathPassesThrough) {
  // `../foo.svg` from baseDir lands outside cwd → passthrough.
  EXPECT_EQ(PrettifyLocalPath("../foo.svg", kCwd), "../foo.svg");
}

TEST(LocalPathDisplayTest, CwdItselfPassesThrough) {
  // Absolute path equal to cwd — not a file, so nothing to prettify.
  EXPECT_EQ(PrettifyLocalPath("/workspace/donner", kCwd), "/workspace/donner");
}

TEST(LocalPathDisplayTest, PathWithTrailingComponentsIsNormalized) {
  EXPECT_EQ(PrettifyLocalPath("/workspace/donner/./a/../b.svg", kCwd), "./b.svg");
}

}  // namespace
}  // namespace donner::editor
