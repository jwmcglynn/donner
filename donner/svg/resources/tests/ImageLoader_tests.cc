#include "donner/svg/resources/ImageLoader.h"

#include <gtest/gtest.h>

#include <cstdint>
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

TEST(ImageLoader, CorruptRasterDataUrlReturnsDataCorrupt) {
  NullResourceLoader resourceLoader;
  ImageLoader imageLoader(resourceLoader);

  ImageLoader::Result result = imageLoader.fromUri("data:image/png;base64,AAAA");

  ASSERT_TRUE(std::holds_alternative<UrlLoaderError>(result));
  EXPECT_EQ(std::get<UrlLoaderError>(result), UrlLoaderError::DataCorrupt);
}

TEST(ImageLoader, OversizedPngHeaderReturnsDataCorruptBeforeDecode) {
  StaticResourceLoader resourceLoader(PngHeaderWithDimensions(20000, 1));
  ImageLoader imageLoader(resourceLoader);

  ImageLoader::Result result = imageLoader.fromUri("oversized.png");

  ASSERT_TRUE(std::holds_alternative<UrlLoaderError>(result));
  EXPECT_EQ(std::get<UrlLoaderError>(result), UrlLoaderError::DataCorrupt);
}

}  // namespace
}  // namespace donner::svg
