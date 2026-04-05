#include "donner/svg/resources/UrlLoader.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::svg {

using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;
using testing::Field;

namespace {

/// A simple in-process resource loader for testing. Returns dummy data for known URLs.
class InProcResourceLoader : public ResourceLoaderInterface {
public:
  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    for (const auto& knownUrl : knownUrls_) {
      if (url == knownUrl) {
        return std::vector<uint8_t>{'t', 'e', 's', 't'};
      }
    }

    return ResourceLoaderError::NotFound;
  }

  /// Add a URL that this loader will recognize and return dummy data for.
  void addKnownUrl(std::string url) { knownUrls_.push_back(std::move(url)); }

private:
  std::vector<std::string> knownUrls_{"test.txt"};
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

/// @test that MIME type is detected from .svg file extension.
TEST(UrlLoader, DetectsSvgMimeType) {
  InProcResourceLoader loader;
  loader.addKnownUrl("icon.svg");
  UrlLoader urlLoader(loader);
  auto result = urlLoader.fromUri("icon.svg");

  EXPECT_THAT(result,
              VariantWith<UrlLoader::Result>(Field(&UrlLoader::Result::mimeType, Eq("image/svg+xml"))));
}

/// @test that MIME type is detected from .svgz file extension.
TEST(UrlLoader, DetectsSvgzMimeType) {
  InProcResourceLoader loader;
  loader.addKnownUrl("icon.svgz");
  UrlLoader urlLoader(loader);
  auto result = urlLoader.fromUri("icon.svgz");

  EXPECT_THAT(result,
              VariantWith<UrlLoader::Result>(Field(&UrlLoader::Result::mimeType, Eq("image/svg+xml"))));
}

/// @test that MIME type is detected from .png file extension.
TEST(UrlLoader, DetectsPngMimeType) {
  InProcResourceLoader loader;
  loader.addKnownUrl("image.png");
  UrlLoader urlLoader(loader);
  auto result = urlLoader.fromUri("image.png");

  EXPECT_THAT(result,
              VariantWith<UrlLoader::Result>(Field(&UrlLoader::Result::mimeType, Eq("image/png"))));
}

/// @test that MIME type is detected from .jpg file extension.
TEST(UrlLoader, DetectsJpegMimeType) {
  InProcResourceLoader loader;
  loader.addKnownUrl("photo.jpg");
  UrlLoader urlLoader(loader);
  auto result = urlLoader.fromUri("photo.jpg");

  EXPECT_THAT(result,
              VariantWith<UrlLoader::Result>(Field(&UrlLoader::Result::mimeType, Eq("image/jpeg"))));
}

/// @test that MIME type detection is case-insensitive.
TEST(UrlLoader, MimeTypeDetectionCaseInsensitive) {
  InProcResourceLoader loader;
  loader.addKnownUrl("icon.SVG");
  UrlLoader urlLoader(loader);
  auto result = urlLoader.fromUri("icon.SVG");

  EXPECT_THAT(result,
              VariantWith<UrlLoader::Result>(Field(&UrlLoader::Result::mimeType, Eq("image/svg+xml"))));
}

/// @test that MIME type detection ignores query strings and fragments in external URLs.
TEST(UrlLoader, MimeTypeDetectionIgnoresQueryAndFragment) {
  InProcResourceLoader loader;
  loader.addKnownUrl("icon.svg?v=2");
  loader.addKnownUrl("icon.svg#element");
  UrlLoader urlLoader(loader);

  auto result1 = urlLoader.fromUri("icon.svg?v=2");
  EXPECT_THAT(result1,
              VariantWith<UrlLoader::Result>(Field(&UrlLoader::Result::mimeType, Eq("image/svg+xml"))));

  auto result2 = urlLoader.fromUri("icon.svg#element");
  EXPECT_THAT(result2,
              VariantWith<UrlLoader::Result>(Field(&UrlLoader::Result::mimeType, Eq("image/svg+xml"))));
}

/// @test that unknown file extensions produce an empty MIME type.
TEST(UrlLoader, UnknownExtensionEmptyMimeType) {
  InProcResourceLoader loader;
  UrlLoader urlLoader(loader);
  auto result = urlLoader.fromUri("test.txt");

  EXPECT_THAT(result,
              VariantWith<UrlLoader::Result>(Field(&UrlLoader::Result::mimeType, Eq(""))));
}

}  // namespace donner::svg
