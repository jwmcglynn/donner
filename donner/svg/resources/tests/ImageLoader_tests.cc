#include "donner/svg/resources/ImageLoader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/tests/Runfiles.h"
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
  const auto* actual = std::get_if<UrlLoaderError>(&result);
  ASSERT_NE(actual, nullptr) << "Result alternative index: " << result.index();
  EXPECT_EQ(*actual, error);
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
  StaticResourceLoader resourceLoader(std::vector<uint8_t>{0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
                                                           0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x1F,
                                                           0x40, 0x1F, 0x08, 0x00});
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
    ExpectImageLoaderError(imageLoader.fromUri("too-large.png"), UrlLoaderError::ResourceTooLarge);
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

TEST(ImageLoader, RejectsDecodedImageBeforeAllocationWhenOverConfiguredLimit) {
  const std::vector<uint8_t> png = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
      0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x04, 0x00, 0x00,
      0x00, 0xB5, 0x1C, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41, 0x54, 0x78,
      0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00, 0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0xA7, 0xDB,
      0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
  };
  StaticResourceLoader resourceLoader(png);
  ImageLoader imageLoader(resourceLoader, UrlLoader::kDefaultMaximumResourceSize, nullptr, 3);

  ExpectImageLoaderError(imageLoader.fromUri("one-pixel.png"), UrlLoaderError::ResourceTooLarge);
}

TEST(ImageLoader, RejectsTinyGifWithProductionDecodeAmplification) {
  const std::string path = Runfiles::instance().Rlocation(
      "donner/svg/resources/tests/image_loader_corpus/regression-gif-declared-canvas.gif");
  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input.is_open()) << path;
  const std::vector<uint8_t> gif(std::istreambuf_iterator<char>{input}, {});
  ASSERT_EQ(gif.size(), 77u);

  StaticResourceLoader resourceLoader(gif);
  ImageLoader imageLoader(resourceLoader);
  ExpectImageLoaderError(imageLoader.fromUri("declared-canvas.gif"),
                         UrlLoaderError::ResourceTooLarge);
}

TEST(ImageLoader, AmplificationBudgetDoesNotRoundUpToConfiguredLimit) {
  std::vector<uint8_t> png = PngHeaderWithDimensions(513, 512);
  png.resize(256, 0);
  StaticResourceLoader resourceLoader(png);
  ImageLoader imageLoader(resourceLoader, UrlLoader::kDefaultMaximumResourceSize, nullptr,
                          513u * 512u * 4u);

  ExpectImageLoaderError(imageLoader.fromUri("declared-canvas.png"),
                         UrlLoaderError::ResourceTooLarge);
}

TEST(ImageLoader, ProductionBudgetDecodesLargeValidPng) {
  const std::string path = Runfiles::instance().Rlocation(
      "donner/svg/resources/tests/image_loader_production_corpus/valid-2048x2048-uniform.png");
  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input.is_open()) << path;
  const std::vector<uint8_t> png(std::istreambuf_iterator<char>{input}, {});
  ASSERT_EQ(png.size(), 16375u);

  StaticResourceLoader resourceLoader(png);
  ImageLoader productionLoader(resourceLoader);
  const auto result = productionLoader.fromUri("large-valid.png");
  const auto* image = std::get_if<ImageResource>(&result);
  ASSERT_NE(image, nullptr) << "Result alternative index: " << result.index();
  EXPECT_EQ(image->data.size(), 16u * 1024u * 1024u);

  ImageLoader fastFuzzBudgetLoader(resourceLoader, UrlLoader::kDefaultMaximumResourceSize, nullptr,
                                   1u * 1024u * 1024u);
  ExpectImageLoaderError(fastFuzzBudgetLoader.fromUri("large-valid.png"),
                         UrlLoaderError::ResourceTooLarge);
}

TEST(ImageLoader, ChargesRawAndDecodedBytesToSharedBudget) {
  const std::vector<uint8_t> png = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
      0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x04, 0x00, 0x00,
      0x00, 0xB5, 0x1C, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41, 0x54, 0x78,
      0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00, 0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0xA7, 0xDB,
      0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
  };

  {
    StaticResourceLoader resourceLoader(png);
    size_t remainingBytes = png.size() + 3;
    ImageLoader imageLoader(resourceLoader, UrlLoader::kDefaultMaximumResourceSize,
                            &remainingBytes);
    ExpectImageLoaderError(imageLoader.fromUri("one-pixel.png"), UrlLoaderError::ResourceTooLarge);
    EXPECT_EQ(remainingBytes, 0u);
  }

  {
    StaticResourceLoader resourceLoader(png);
    size_t remainingBytes = png.size() + 4;
    ImageLoader imageLoader(resourceLoader, UrlLoader::kDefaultMaximumResourceSize,
                            &remainingBytes);
    ASSERT_TRUE(std::holds_alternative<ImageResource>(imageLoader.fromUri("one-pixel.png")));
    EXPECT_EQ(remainingBytes, 0u);
  }
}

TEST(ImageLoader, ReturnsDataCorruptWhenInfoSucceedsButDecodeFails) {
  StaticResourceLoader resourceLoader(PngHeaderWithDimensions(1, 1));
  ImageLoader imageLoader(resourceLoader);

  ExpectImageLoaderError(imageLoader.fromUri("missing-idat.png"), UrlLoaderError::DataCorrupt);
}

TEST(ImageLoader, RejectsPngWithTruncatedDeclaredIdat) {
  // Regression: a 69-byte truncated PNG whose second IDAT chunk declares a
  // 0x40000000-byte payload while only four payload bytes are present.
  // stb_image sizes its IDAT accumulation buffer from the declared length
  // before checking that the data exists, so the decode requests a 2 GiB
  // allocation and aborts libFuzzer's memory limit (image_loader_fuzzer OOM).
  // The checked-in corpus seed `regression-png-truncated-idat.png` is the
  // red-to-green reproducer; this test pins the loader's rejection contract.
  StaticResourceLoader resourceLoader(std::vector<uint8_t>{
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
      0x00, 0x00, 0x00, 0x0D, 'I',  'H',  'D',  'R',   // IHDR, 13 bytes
      0x00, 0x00, 0x00, 0x0B,                          // width 11
      0x00, 0x00, 0x04, 0x05,                          // height 1029
      0x08, 0x02, 0x00, 0x00, 0x00,                    // bit depth 8, color type 2
      0x90, 0x77, 0x53, 0xDE,                          // IHDR CRC
      0x00, 0x00, 0x00, 0x0C, 'I',  'D',  'A',  'T',   // IDAT, 12 bytes
      0x78, 0x9C, 0x6B, 0xF8, 0xCF, 0xC0, 0x00, 0x01,
      0x03, 0x01, 0xE5, 0x00, 0xC9, 0xDE, 0x92, 0xEF,  // IDAT CRC
      0x40, 0x00, 0x00, 0x00, 'I',  'D',  'A',  'T',   // IDAT declaring 0x40000000
      0x78, 0x42, 0x60, 0x82,                          // only four payload bytes present
  });
  ImageLoader imageLoader(resourceLoader);

  ExpectImageLoaderError(imageLoader.fromUri("truncated-idat.png"), UrlLoaderError::DataCorrupt);
}

TEST(ImageLoader, LoadsPngWithAncillaryChunkAndSplitIdat) {
  // The declared-length walk must accept well-formed PNGs that carry ancillary
  // chunks and split the zlib stream across multiple IDAT chunks. Both IDAT
  // chunks carry zero CRCs; stb_image reads and discards chunk CRCs without
  // verifying them.
  StaticResourceLoader resourceLoader(std::vector<uint8_t>{
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
      0x00, 0x00, 0x00, 0x0D, 'I',  'H',  'D',  'R',   // IHDR, 13 bytes
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  // 1x1
      0x08, 0x04, 0x00, 0x00, 0x00,                    // bit depth 8, grayscale+alpha
      0xB5, 0x1C, 0x0C, 0x02,                          // IHDR CRC
      0x00, 0x00, 0x00, 0x00, 't',  'E',  'X',  't',   // empty tEXt chunk
      0x00, 0x00, 0x00, 0x00,                          // CRC (stb_image does not verify it)
      0x00, 0x00, 0x00, 0x05, 'I',  'D',  'A',  'T',   // first IDAT, 5 bytes
      0x78, 0xDA, 0x63, 0xFC, 0xFF, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x06, 'I',  'D',  'A',  'T',  // second IDAT, 6 bytes
      0x1F, 0x00, 0x03, 0x03, 0x02, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 'I',  'E',  'N',  'D',  // IEND
      0xAE, 0x42, 0x60, 0x82,                               // IEND CRC
  });
  ImageLoader imageLoader(resourceLoader);

  ImageLoader::Result result = imageLoader.fromUri("split-idat.png");

  ASSERT_TRUE(std::holds_alternative<ImageResource>(result));
  const ImageResource& image = std::get<ImageResource>(result);
  EXPECT_EQ(image.width, 1);
  EXPECT_EQ(image.height, 1);
  EXPECT_EQ(image.data.size(), 4u);
}

TEST(ImageLoader, ReturnsUrlLoaderErrors) {
  NullResourceLoader resourceLoader;
  ImageLoader imageLoader(resourceLoader);

  ExpectImageLoaderError(imageLoader.fromUri("missing.png"), UrlLoaderError::NotFound);
}

}  // namespace
}  // namespace donner::svg
