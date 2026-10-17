#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace {

TEST(LibcCompat, FilesystemCopyPreservesBytes) {
  const char* temporaryDirectory = std::getenv("TEST_TMPDIR");
  ASSERT_THAT(temporaryDirectory, testing::NotNull());
  const std::filesystem::path directory(temporaryDirectory);
  const auto source = directory / "copy-source";
  const auto destination = directory / "copy-destination";
  const std::string expected = "file copy through the compatibility shim";
  {
    std::ofstream output(source, std::ios::binary);
    output << expected;
    ASSERT_THAT(output.good(), testing::IsTrue());
  }
  std::error_code error;
  EXPECT_THAT(std::filesystem::copy_file(source, destination, error), testing::IsTrue());
  ASSERT_THAT(error.value(), testing::Eq(0)) << error.message();
  std::ifstream input(destination, std::ios::binary);
  const std::string actual(std::istreambuf_iterator<char>{input}, {});
  EXPECT_THAT(actual, testing::Eq(expected));
}

TEST(LibcCompat, RangeCopyPreservesOffsetsEofAndErrors) {
  const char* temporaryDirectory = std::getenv("TEST_TMPDIR");
  ASSERT_THAT(temporaryDirectory, testing::NotNull());
  const std::filesystem::path directory(temporaryDirectory);
  using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
  File source(std::fopen((directory / "range-source").c_str(), "w+b"), std::fclose);
  File destination(std::fopen((directory / "range-destination").c_str(), "w+b"), std::fclose);
  ASSERT_THAT(source, testing::NotNull());
  ASSERT_THAT(destination, testing::NotNull());
  ASSERT_EQ(std::fwrite("abcdef", 1, 6, source.get()), 6u);
  ASSERT_EQ(std::fflush(source.get()), 0);
  off_t sourceOffset = 2;
  off_t destinationOffset = 0;
  ASSERT_EQ(copy_file_range(fileno(source.get()), &sourceOffset, fileno(destination.get()),
                            &destinationOffset, 3, 0),
            3);
  EXPECT_EQ(sourceOffset, 5);
  EXPECT_EQ(destinationOffset, 3);
  char actual[3] = {};
  ASSERT_EQ(std::fread(actual, 1, sizeof(actual), destination.get()), sizeof(actual));
  EXPECT_THAT(std::string(actual, sizeof(actual)), testing::Eq("cde"));
  sourceOffset = 6;
  EXPECT_EQ(copy_file_range(fileno(source.get()), &sourceOffset, fileno(destination.get()),
                            &destinationOffset, 1, 0),
            0);
  EXPECT_EQ(sourceOffset, 6);
  EXPECT_EQ(destinationOffset, 3);
  errno = 0;
  const ssize_t invalidResult =
      copy_file_range(-1, nullptr, fileno(destination.get()), nullptr, 1, 0);
  const int invalidError = errno;
  EXPECT_EQ(invalidResult, -1);
  EXPECT_EQ(invalidError, EBADF);
}

}  // namespace
