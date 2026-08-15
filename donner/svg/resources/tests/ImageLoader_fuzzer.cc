#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "donner/svg/resources/ImageLoader.h"

namespace donner::svg {

namespace {

/// Resource loader that returns the fuzz input verbatim for any URL. This is used to feed raw
/// bytes directly into the raster image decoder (stb_image), which is the highest-risk untrusted
/// input surface in this directory.
class FuzzResourceLoader : public ResourceLoaderInterface {
public:
  explicit FuzzResourceLoader(std::vector<uint8_t> data) : data_(std::move(data)) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view /*url*/) override {
    return data_;
  }

private:
  std::vector<uint8_t> data_;
};

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::vector<uint8_t> bytes(data, data + size);

  size_t maximumDecodedImageSize = ImageLoader::kDefaultMaximumDecodedImageSize;
  if (size >= 2 && (data[size - 1] & 1u) != 0) {
    maximumDecodedImageSize = 4u * (static_cast<size_t>(data[size - 2]) + 1u);
  }

  FuzzResourceLoader loader(bytes);
  const bool canAccountInput = size <= UrlLoader::kDefaultMaximumResourceSize &&
                               size <= std::numeric_limits<size_t>::max() - maximumDecodedImageSize;
  size_t remainingResourceBytes =
      canAccountInput ? size + maximumDecodedImageSize : maximumDecodedImageSize;
  ImageLoader imageLoader(loader, UrlLoader::kDefaultMaximumResourceSize, &remainingResourceBytes,
                          maximumDecodedImageSize);

  // Use a fixed URL with a raster image extension. The parser gates supported magic bytes before
  // stb_image, so this exercises the PNG, JPEG, and GIF decoders on arbitrary fuzz bytes.
  auto result = imageLoader.fromUri("fuzz-input.png");
  if (std::holds_alternative<ImageResource>(result)) {
    const ImageResource& image = std::get<ImageResource>(result);
    if (image.data.size() > maximumDecodedImageSize || !canAccountInput ||
        remainingResourceBytes != maximumDecodedImageSize - image.data.size()) {
      std::abort();
    }
  }

  return 0;
}

}  // namespace donner::svg
