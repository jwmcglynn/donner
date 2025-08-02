#include "donner/svg/resources/UrlLoader.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::svg {

using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;
using testing::Field;

namespace {

/// A simple in-process resource loader for testing.
class InProcResourceLoader : public ResourceLoaderInterface {
public:
  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    if (url == "test.txt") {
      return std::vector<uint8_t>{'t', 'e', 's', 't'};
    }

    return ResourceLoaderError::NotFound;
  }
};

/// A helper matcher that extracts the alternative T from a std::variant.
template <typename T, typename Matcher>
auto VariantWith(const Matcher& matcher) {
  return ::testing::Truly([matcher](const auto& variant) -> bool {
    if (const T* p = std::get_if<T>(&variant)) {
      return ::testing::Matches(matcher)(*p);
    }
    return false;
  });
}

}  // namespace

/// @test that the UrlLoaderError enum can be converted to a string.
TEST(UrlLoader, UrlLoaderErrorToString) {
  EXPECT_EQ(ToString(UrlLoaderError::NotFound), "File not found");
  EXPECT_EQ(ToString(UrlLoaderError::UnsupportedFormat), "Unsupported format");
  EXPECT_EQ(ToString(UrlLoaderError::InvalidDataUrl), "Invalid data URL");
  EXPECT_EQ(ToString(UrlLoaderError::DataCorrupt), "Data corrupted");
}

/// @test that a valid file URI is correctly fetched.
TEST(UrlLoader, FetchExternalResource) {
  InProcResourceLoader loader;
  UrlLoader urlLoader(loader);
  auto result = urlLoader.fromUri("test.txt");

  EXPECT_THAT(result, VariantWith<UrlLoader::Result>(
                          AllOf(Field(&UrlLoader::Result::mimeType, Eq("")),
                                Field(&UrlLoader::Result::data, ElementsAre('t', 'e', 's', 't')))));
}

/// @test that an invalid file URI is handled appropriately.
TEST(UrlLoader, FetchNonExistentResource) {
  InProcResourceLoader loader;
  UrlLoader urlLoader(loader);
  auto result = urlLoader.fromUri("test2.txt");

  EXPECT_THAT(result, VariantWith<UrlLoaderError>(UrlLoaderError::NotFound));
}

/// @test that a valid base64 data URL is correctly decoded.
TEST(UrlLoader, FetchDataUrlBase64) {
  InProcResourceLoader loader;
  UrlLoader urlLoader(loader);
  // "dGVzdA==" is the base64 encoding of "test".
  auto result = urlLoader.fromUri("data:text/plain;base64,dGVzdA==");

  EXPECT_THAT(result, VariantWith<UrlLoader::Result>(
                          AllOf(Field(&UrlLoader::Result::mimeType, Eq("text/plain")),
                                Field(&UrlLoader::Result::data, ElementsAre('t', 'e', 's', 't')))));
}

/// @test that a valid URL-encoded data URL with an explicit MIME type is decoded.
TEST(UrlLoader, FetchDataUrlUrlEncodedWithMime) {
  InProcResourceLoader loader;
  UrlLoader urlLoader(loader);
  // Here, the comma separates the MIME type from the data.
  // "hello%20world" should URL-decode to "hello world".
  auto result = urlLoader.fromUri("data:text/plain,hello%20world");

  EXPECT_THAT(result, VariantWith<UrlLoader::Result>(AllOf(
                          Field(&UrlLoader::Result::mimeType, Eq("text/plain")),
                          Field(&UrlLoader::Result::data, ElementsAre('h', 'e', 'l', 'l', 'o', ' ',
                                                                      'w', 'o', 'r', 'l', 'd')))));
}

/// @test that a valid URL-encoded data URL without an explicit MIME type is decoded.
TEST(UrlLoader, FetchDataUrlUrlEncodedNoMime) {
  InProcResourceLoader loader;
  UrlLoader urlLoader(loader);
  // With no semicolon, the entire string is treated as URL-encoded data, but the comma is required.
  auto result = urlLoader.fromUri("data:,hello%20world");

  const std::string_view expectedStr = "hello world";
  EXPECT_THAT(result,
              VariantWith<UrlLoader::Result>(
                  AllOf(Field(&UrlLoader::Result::mimeType, Eq("")),
                        Field(&UrlLoader::Result::data, testing::ElementsAreArray(expectedStr)))));
}

/// @test that an invalid base64 data URL is handled appropriately.
TEST(UrlLoader, FetchDataUrlInvalidBase64) {
  InProcResourceLoader loader;
  UrlLoader urlLoader(loader);
  // "!!!!" is not valid base64 and should result in an error.
  auto result = urlLoader.fromUri("data:image/png;base64,!!!!");

  EXPECT_THAT(result, VariantWith<UrlLoaderError>(UrlLoaderError::InvalidDataUrl));
}

}  // namespace donner::svg
