#include "donner/editor/FileDialogState.h"

#include <gtest/gtest.h>

namespace donner::editor {

TEST(SvgFileDialogFilters, ExposesSvgExtension) {
  const std::vector<FileDialogFilter> filters = SvgFileDialogFilters();
  ASSERT_EQ(filters.size(), 1u);
  EXPECT_EQ(filters[0].description, "SVG Image");
  ASSERT_EQ(filters[0].extensions.size(), 1u);
  EXPECT_EQ(filters[0].extensions[0], "svg");
}

TEST(FileDialogState, DefaultDirectoryEmptyWithNoContext) {
  const FileDialogState state;
  EXPECT_FALSE(state.defaultDirectory(std::nullopt).has_value());
}

TEST(FileDialogState, DefaultDirectoryDerivedFromCurrentFile) {
  const FileDialogState state;
  const auto dir = state.defaultDirectory(std::optional<std::string>("/home/user/art/a.svg"));
  ASSERT_TRUE(dir.has_value());
  EXPECT_EQ(*dir, "/home/user/art");
}

TEST(FileDialogState, DefaultDirectoryEmptyForBareFilename) {
  const FileDialogState state;
  EXPECT_FALSE(state.defaultDirectory(std::optional<std::string>("a.svg")).has_value());
}

TEST(FileDialogState, RememberedDirectoryWinsOverCurrentFile) {
  FileDialogState state;
  state.noteChosenPath("/tmp/chosen/foo.svg");
  const auto dir = state.defaultDirectory(std::optional<std::string>("/home/user/art/a.svg"));
  ASSERT_TRUE(dir.has_value());
  EXPECT_EQ(*dir, "/tmp/chosen");
}

TEST(FileDialogState, NoteChosenPathRecordsRecentsNewestFirst) {
  FileDialogState state;
  state.noteChosenPath("/a/one.svg");
  state.noteChosenPath("/a/two.svg");
  state.noteChosenPath("/a/three.svg");
  ASSERT_EQ(state.recentFiles().size(), 3u);
  EXPECT_EQ(state.recentFiles()[0], "/a/three.svg");
  EXPECT_EQ(state.recentFiles()[1], "/a/two.svg");
  EXPECT_EQ(state.recentFiles()[2], "/a/one.svg");
}

TEST(FileDialogState, RepeatedPathMovesToFrontWithoutDuplicating) {
  FileDialogState state;
  state.noteChosenPath("/a/one.svg");
  state.noteChosenPath("/a/two.svg");
  state.noteChosenPath("/a/one.svg");
  ASSERT_EQ(state.recentFiles().size(), 2u);
  EXPECT_EQ(state.recentFiles()[0], "/a/one.svg");
  EXPECT_EQ(state.recentFiles()[1], "/a/two.svg");
}

TEST(FileDialogState, RecentsBoundedByMax) {
  FileDialogState state(/*maxRecents=*/2);
  state.noteChosenPath("/a/one.svg");
  state.noteChosenPath("/a/two.svg");
  state.noteChosenPath("/a/three.svg");
  ASSERT_EQ(state.recentFiles().size(), 2u);
  EXPECT_EQ(state.recentFiles()[0], "/a/three.svg");
  EXPECT_EQ(state.recentFiles()[1], "/a/two.svg");
}

TEST(FileDialogState, EmptyPathIgnored) {
  FileDialogState state;
  state.noteChosenPath("");
  EXPECT_TRUE(state.recentFiles().empty());
  EXPECT_FALSE(state.lastDirectory().has_value());
}

}  // namespace donner::editor
