#include "donner/svg/resources/NullResourceLoader.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::svg {

TEST(NullResourceLoaderTests, AlwaysReturnsError) {
  NullResourceLoader loader;

  // Test various path types
  EXPECT_THAT(loader.fetchExternalResource("test.txt"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
  EXPECT_THAT(loader.fetchExternalResource(""),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
  EXPECT_THAT(loader.fetchExternalResource("../test.txt"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
  EXPECT_THAT(loader.fetchExternalResource("/absolute/path/test.txt"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
  EXPECT_THAT(loader.fetchExternalResource("http://example.com/resource.svg"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
  EXPECT_THAT(loader.fetchExternalResource("https://example.com/resource.svg"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
  EXPECT_THAT(loader.fetchExternalResource("file:///path/to/file.txt"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
  EXPECT_THAT(loader.fetchExternalResource("data:text/plain;base64,SGVsbG8gV29ybGQ="),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));

  std::string longPath(1000, 'a');
  longPath += ".txt";
  EXPECT_THAT(loader.fetchExternalResource(longPath),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));

  EXPECT_THAT(loader.fetchExternalResource("file with spaces & symbols!@#$.txt"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
  EXPECT_THAT(loader.fetchExternalResource("файл.txt"),
              testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));

  // Test multiple consecutive calls
  for (int i = 0; i < 10; ++i) {
    EXPECT_THAT(loader.fetchExternalResource("test" + std::to_string(i) + ".txt"),
                testing::VariantWith<ResourceLoaderError>(ResourceLoaderError::NotFound));
  }
}

}  // namespace donner::svg
