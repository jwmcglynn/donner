#include "donner/base/FileUtils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace donner {
namespace {

using ::testing::VariantWith;

std::filesystem::path WriteTestFile(std::string_view name, std::string_view contents) {
  const std::filesystem::path path = std::filesystem::path(testing::TempDir()) / name;
  std::ofstream output(path, std::ios::binary);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return path;
}

TEST(FileUtilsTest, ReadsFileAtExactLimit) {
  const std::filesystem::path path = WriteTestFile("bounded-exact.txt", "test");
  EXPECT_THAT(ReadFileBounded(path, 4), VariantWith<std::string>("test"));
}

TEST(FileUtilsTest, RejectsFileOverLimit) {
  const std::filesystem::path path = WriteTestFile("bounded-too-large.txt", "tests");
  EXPECT_THAT(ReadFileBounded(path, 4), VariantWith<FileReadError>(FileReadError::TooLarge));
}

TEST(FileUtilsTest, ReportsMissingFile) {
  const std::filesystem::path path =
      std::filesystem::path(testing::TempDir()) / "bounded-missing.txt";
  EXPECT_THAT(ReadFileBounded(path, 4), VariantWith<FileReadError>(FileReadError::OpenFailed));
}

#ifndef _WIN32
TEST(FileUtilsTest, ReadsRegularFileThroughSymlink) {
  const std::filesystem::path target = WriteTestFile("bounded-symlink-target.txt", "test");
  const std::filesystem::path link =
      std::filesystem::path(testing::TempDir()) / "bounded-symlink.txt";
  std::error_code error;
  std::filesystem::create_symlink(target, link, error);
  ASSERT_FALSE(error) << error.message();
  EXPECT_THAT(ReadFileBounded(link, 4), VariantWith<std::string>("test"));
}

TEST(FileUtilsTest, RejectsFifoWithoutBlocking) {
  const std::filesystem::path path =
      std::filesystem::path(testing::TempDir()) / "bounded-input.fifo";
  ASSERT_EQ(mkfifo(path.c_str(), 0600), 0);
  EXPECT_THAT(ReadFileBounded(path, 4), VariantWith<FileReadError>(FileReadError::OpenFailed));
}
#endif

}  // namespace
}  // namespace donner
