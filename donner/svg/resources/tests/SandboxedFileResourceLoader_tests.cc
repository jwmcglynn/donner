#include "donner/svg/resources/SandboxedFileResourceLoader.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace donner::svg {

class SandboxedFileResourceLoaderTests : public testing::Test {
public:
  SandboxedFileResourceLoaderTests() {
    const std::string tmpDir = testing::TempDir();

    root_ = std::filesystem::path(tmpDir) / "root";
    secondaryDir_ = std::filesystem::path(tmpDir) / "secondary";

    std::filesystem::create_directories(root_);
    std::filesystem::create_directories(secondaryDir_);
  }

  void createTestFileUnder(const std::filesystem::path& dir, const std::string& filename) {
    std::ofstream file(dir / filename);
    file << "test" << '\0';
    file.close();
  }

protected:
  std::filesystem::path root_;
  std::filesystem::path secondaryDir_;
};

TEST_F(SandboxedFileResourceLoaderTests, LoadFileFromRoot) {
  createTestFileUnder(root_, "test.txt");

  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg");
  auto data = loader.fetchExternalResource("test.txt");
  EXPECT_THAT(data, testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAreArray("test")));
}

TEST_F(SandboxedFileResourceLoaderTests, RootDoesNotExit) {
  SandboxedFileResourceLoader loader(root_ / "doesnotexist", root_ / "doc.svg");
  EXPECT_THAT(loader.fetchExternalResource("test.txt"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
}

TEST_F(SandboxedFileResourceLoaderTests, LoadFileFromSubdirectory) {
  std::filesystem::create_directories(root_ / "subdir");
  createTestFileUnder(root_ / "subdir", "test.txt");

  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg");
  auto data = loader.fetchExternalResource("subdir/test.txt");
  EXPECT_THAT(data, testing::VariantWith<std::vector<uint8_t>>(testing::ElementsAreArray("test")));
}

TEST_F(SandboxedFileResourceLoaderTests, AccessNonExistentFile) {
  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg");

  auto data = loader.fetchExternalResource("test2.txt");
  EXPECT_THAT(data, testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
}

TEST_F(SandboxedFileResourceLoaderTests, AccessDirectoryReturnsNotFound) {
  std::filesystem::create_directories(root_ / "subdir");
  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg");

  auto data = loader.fetchExternalResource("subdir");
  EXPECT_THAT(data, testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
}

#ifndef _WIN32
TEST_F(SandboxedFileResourceLoaderTests, AccessFifoReturnsWithoutBlocking) {
  const std::filesystem::path fifoPath = root_ / "resource.fifo";
  ASSERT_EQ(mkfifo(fifoPath.c_str(), 0600), 0);
  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg");

  EXPECT_THAT(loader.fetchExternalResource("resource.fifo"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
}
#endif

TEST_F(SandboxedFileResourceLoaderTests, AccessOutsideSandbox) {
  createTestFileUnder(secondaryDir_, "test.txt");
  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg");

  EXPECT_THAT(loader.fetchExternalResource("../secondary/test.txt"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::SandboxViolation));

  EXPECT_THAT(loader.fetchExternalResource((secondaryDir_ / "test.txt").string()),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::SandboxViolation));
}

TEST_F(SandboxedFileResourceLoaderTests, RejectsEmbeddedNullBeforeFilesystemTraversal) {
  createTestFileUnder(secondaryDir_, "secret.txt");
  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg");
  constexpr char kEmbeddedNullPath[] = "..\0ignored/secondary/secret.txt";
  const std::string path(kEmbeddedNullPath, sizeof(kEmbeddedNullPath) - 1);

  EXPECT_THAT(loader.fetchExternalResource(path),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::SandboxViolation));
}

TEST_F(SandboxedFileResourceLoaderTests, RejectsInvalidUtf8BeforeFilesystemConversion) {
  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg");
  EXPECT_THAT(loader.fetchExternalResource(std::string("invalid-\xED\xA0\x80.bin", 15)),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::SandboxViolation));
}

TEST_F(SandboxedFileResourceLoaderTests, RejectsOversizedPathRepresentations) {
  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg");
  EXPECT_THAT(loader.fetchExternalResource(
                  std::string(SandboxedFileResourceLoader::kMaximumPathBytes + 1, 'a')),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::SandboxViolation));

  std::string components;
  for (std::size_t i = 0; i <= SandboxedFileResourceLoader::kMaximumPathComponents; ++i) {
    components += "a/";
  }
  EXPECT_THAT(loader.fetchExternalResource(components),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::SandboxViolation));
}

TEST_F(SandboxedFileResourceLoaderTests, RejectsSiblingWhoseNameSharesRootPrefix) {
  const std::filesystem::path prefixSibling = root_.parent_path() / "root-backup";
  std::filesystem::create_directories(prefixSibling);
  createTestFileUnder(prefixSibling, "secret.txt");

  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg");
  EXPECT_THAT(loader.fetchExternalResource((prefixSibling / "secret.txt").string()),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::SandboxViolation));
}

TEST_F(SandboxedFileResourceLoaderTests, RejectsSymlinkThatEscapesRoot) {
  createTestFileUnder(secondaryDir_, "secret.txt");
  std::error_code error;
  std::filesystem::create_directory_symlink(secondaryDir_, root_ / "escape", error);
  if (error) {
    GTEST_SKIP() << "Could not create test symlink: " << error.message();
  }

  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg");
  EXPECT_THAT(loader.fetchExternalResource("escape/secret.txt"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::SandboxViolation));
}

TEST_F(SandboxedFileResourceLoaderTests, RejectsResourceLargerThanConfiguredLimit) {
  createTestFileUnder(root_, "test.txt");

  SandboxedFileResourceLoader loader(root_, root_ / "doc.svg", 4);
  EXPECT_THAT(loader.fetchExternalResource("test.txt"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::TooLarge));
}

}  // namespace donner::svg
