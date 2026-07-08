#include "donner/svg/resources/ImageLoader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "donner/svg/resources/NullResourceLoader.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {
namespace {

class StaticResourceLoader : public ResourceLoaderInterface {
public:
  explicit StaticResourceLoader(std::vector<uint8_t> data) : data_(std::move(data)) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view /*url*/) override {
    return data_;
  }

private:
  std::vector<uint8_t> data_;
};

std::vector<uint8_t> PngHeaderWithDimensions(int width, int height) {
  const auto appendUint32 = [](std::vector<uint8_t>& data, uint32_t value) {
    data.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
    data.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
    data.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    data.push_back(static_cast<uint8_t>(value & 0xFFu));
  };

  std::vector<uint8_t> data{
      0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au,
  };
  appendUint32(data, 13u);
  data.insert(data.end(), {'I', 'H', 'D', 'R'});
  appendUint32(data, static_cast<uint32_t>(width));
  appendUint32(data, static_cast<uint32_t>(height));
  data.insert(data.end(), {8u, 6u, 0u, 0u, 0u});
  appendUint32(data, 0u);
  appendUint32(data, 0u);
  data.insert(data.end(), {'I', 'E', 'N', 'D'});
  appendUint32(data, 0u);
  return data;
}

void ExpectImageLoaderError(const ImageLoader::Result& result, UrlLoaderError error) {
  ASSERT_TRUE(std::holds_alternative<UrlLoaderError>(result));
  EXPECT_EQ(std::get<UrlLoaderError>(result), error);
}

TEST(ImageLoader, CorruptRasterDataUrlReturnsDataCorrupt) {
  NullResourceLoader resourceLoader;
  ImageLoader imageLoader(resourceLoader);

  ImageLoader::Result result = imageLoader.fromUri("data:image/png;base64,AAAA");

  ExpectImageLoaderError(result, UrlLoaderError::DataCorrupt);
}

TEST(ImageLoader, RejectsMagiclessTgaDecodeBomb) {
  // Regression: an 18-byte TGA header declaring 8000x8000. TGA has no magic
  // bytes, so stb auto-detects it and its decoder iterates the full declared
  // pixel grid regardless of input size (CPU-exhaustion decode-bomb). Only
  // PNG/JPEG/GIF are supported, so this must be rejected before any decode.
  StaticResourceLoader resourceLoader(std::vector<uint8_t>{0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
                                                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                           0x40, 0x1F, 0x40, 0x1F, 0x08, 0x00});
  ImageLoader imageLoader(resourceLoader);

  ImageLoader::Result result = imageLoader.fromUri("bomb");

  ExpectImageLoaderError(result, UrlLoaderError::DataCorrupt);
}

TEST(ImageLoader, OversizedPngHeaderReturnsDataCorruptBeforeDecode) {
  StaticResourceLoader resourceLoader(PngHeaderWithDimensions(20000, 1));
  ImageLoader imageLoader(resourceLoader);

  ImageLoader::Result result = imageLoader.fromUri("oversized.png");

  ExpectImageLoaderError(result, UrlLoaderError::DataCorrupt);
}

TEST(ImageLoader, RejectsUnsupportedRasterMimeType) {
  NullResourceLoader resourceLoader;
  ImageLoader imageLoader(resourceLoader);

  ImageLoader::Result result = imageLoader.fromUri("data:image/webp;base64,AAAA");

  ExpectImageLoaderError(result, UrlLoaderError::UnsupportedFormat);
}

TEST(ImageLoader, AllowsSupportedRasterMimeTypesBeforeDecode) {
  NullResourceLoader resourceLoader;
  ImageLoader imageLoader(resourceLoader);

  ExpectImageLoaderError(imageLoader.fromUri("data:image/jpeg;base64,AAAA"),
                         UrlLoaderError::DataCorrupt);
  ExpectImageLoaderError(imageLoader.fromUri("data:image/jpg;base64,AAAA"),
                         UrlLoaderError::DataCorrupt);
  ExpectImageLoaderError(imageLoader.fromUri("data:image/gif;base64,AAAA"),
                         UrlLoaderError::DataCorrupt);
}

TEST(ImageLoader, LoadsPngDataUrl) {
  NullResourceLoader resourceLoader;
  ImageLoader imageLoader(resourceLoader);

  ImageLoader::Result result = imageLoader.fromUri(
      "data:image/png;base64,"
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+"
      "/p9sAAAAASUVORK5CYII=");

  ASSERT_TRUE(std::holds_alternative<ImageResource>(result));
  const ImageResource& image = std::get<ImageResource>(result);
  EXPECT_EQ(image.width, 1);
  EXPECT_EQ(image.height, 1);
  EXPECT_EQ(image.data.size(), 4u);
}

TEST(ImageLoader, RoutesExplicitSvgMimeTypeToSvgContent) {
  NullResourceLoader resourceLoader;
  ImageLoader imageLoader(resourceLoader);

  ImageLoader::Result result = imageLoader.fromUri("data:image/svg+xml,%3Csvg/%3E");

  ASSERT_TRUE(std::holds_alternative<SvgImageContent>(result));
  const SvgImageContent& svg = std::get<SvgImageContent>(result);
  EXPECT_EQ(std::string(svg.data.begin(), svg.data.end()), "<svg/>");
}

TEST(ImageLoader, SniffsSvgContentWhenMimeTypeIsEmpty) {
  NullResourceLoader resourceLoader;
  ImageLoader imageLoader(resourceLoader);

  ImageLoader::Result result = imageLoader.fromUri("data:,%20%09%0D%0A%3Csvg/%3E");

  ASSERT_TRUE(std::holds_alternative<SvgImageContent>(result));
  const SvgImageContent& svg = std::get<SvgImageContent>(result);
  EXPECT_EQ(std::string(svg.data.begin(), svg.data.end()), " \t\r\n<svg/>");
}

TEST(ImageLoader, SniffsSvgzMagicWhenMimeTypeIsEmpty) {
  StaticResourceLoader resourceLoader({0x1F, 0x8B, 0x08});
  ImageLoader imageLoader(resourceLoader);

  ImageLoader::Result result = imageLoader.fromUri("image.bin");

  ASSERT_TRUE(std::holds_alternative<SvgImageContent>(result));
  const SvgImageContent& svg = std::get<SvgImageContent>(result);
  EXPECT_EQ(svg.data, (std::vector<uint8_t>{0x1F, 0x8B, 0x08}));
}

TEST(ImageLoader, RejectsPartialSvgzMagicAsRasterData) {
  StaticResourceLoader resourceLoader({0x1F, 0x00});
  ImageLoader imageLoader(resourceLoader);

  ExpectImageLoaderError(imageLoader.fromUri("image.bin"), UrlLoaderError::DataCorrupt);
}

TEST(ImageLoader, EmptyMimeTypeNonSvgContentFallsThroughToRasterDecode) {
  NullResourceLoader resourceLoader;
  ImageLoader imageLoader(resourceLoader);

  ExpectImageLoaderError(imageLoader.fromUri("data:,%20%09%0D%0A"), UrlLoaderError::DataCorrupt);
  ExpectImageLoaderError(imageLoader.fromUri("data:,A"), UrlLoaderError::DataCorrupt);
  ExpectImageLoaderError(imageLoader.fromUri("data:,AB"), UrlLoaderError::DataCorrupt);
}

TEST(ImageLoader, RejectsPngHeadersWithInvalidDimensions) {
  {
    StaticResourceLoader resourceLoader(PngHeaderWithDimensions(1, 20000));
    ImageLoader imageLoader(resourceLoader);
    ExpectImageLoaderError(imageLoader.fromUri("too-tall.png"), UrlLoaderError::DataCorrupt);
  }
  {
    StaticResourceLoader resourceLoader(PngHeaderWithDimensions(16384, 16384));
    ImageLoader imageLoader(resourceLoader);
    ExpectImageLoaderError(imageLoader.fromUri("too-large.png"), UrlLoaderError::DataCorrupt);
  }
  {
    StaticResourceLoader resourceLoader(PngHeaderWithDimensions(0, 1));
    ImageLoader imageLoader(resourceLoader);
    ExpectImageLoaderError(imageLoader.fromUri("zero-width.png"), UrlLoaderError::DataCorrupt);
  }
  {
    StaticResourceLoader resourceLoader(PngHeaderWithDimensions(1, 0));
    ImageLoader imageLoader(resourceLoader);
    ExpectImageLoaderError(imageLoader.fromUri("zero-height.png"), UrlLoaderError::DataCorrupt);
  }
}

TEST(ImageLoader, ReturnsDataCorruptWhenInfoSucceedsButDecodeFails) {
  StaticResourceLoader resourceLoader(PngHeaderWithDimensions(1, 1));
  ImageLoader imageLoader(resourceLoader);

  ExpectImageLoaderError(imageLoader.fromUri("missing-idat.png"), UrlLoaderError::DataCorrupt);
}

TEST(ImageLoader, ReturnsUrlLoaderErrors) {
  NullResourceLoader resourceLoader;
  ImageLoader imageLoader(resourceLoader);

  ExpectImageLoaderError(imageLoader.fromUri("missing.png"), UrlLoaderError::NotFound);
}

}  // namespace
}  // namespace donner::svg
