#include "donner/svg/renderer/tests/TestFontDirectory.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "donner/base/tests/TestTempDir.h"

namespace donner::svg {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;

/// File names of @p paths, in order.
std::vector<std::string> FileNames(const std::vector<std::filesystem::path>& paths) {
  std::vector<std::string> names;
  for (const std::filesystem::path& path : paths) {
    names.push_back(path.filename().string());
  }
  return names;
}

TEST(TestFontDirectory, ListsFontFilesSortedByName) {
  const std::filesystem::path directory = TestTempDir() / "sorted-fonts";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  // Created out of order; the listing must not depend on creation or directory order.
  for (const char* name : {"Zeta.ttf", "notes.txt", "Beta.otf", "alpha.ttf", "Amiri.ttf"}) {
    std::ofstream(directory / name) << "x";
  }

  std::error_code error;
  const std::vector<std::filesystem::path> paths = ListFontFilesSorted(directory, error);
  EXPECT_THAT(error, Eq(std::error_code()));
  EXPECT_THAT(FileNames(paths), ElementsAre("Amiri.ttf", "Beta.otf", "Zeta.ttf", "alpha.ttf"));
}

TEST(TestFontDirectory, MissingDirectoryReportsAnError) {
  std::error_code error;
  const std::vector<std::filesystem::path> paths =
      ListFontFilesSorted(TestTempDir() / "no-such-font-directory", error);
  EXPECT_THAT(static_cast<bool>(error), Eq(true));
  EXPECT_THAT(paths, IsEmpty());
}

}  // namespace
}  // namespace donner::svg
