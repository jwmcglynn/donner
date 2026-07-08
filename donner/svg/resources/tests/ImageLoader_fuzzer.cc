#include "donner/svg/resources/ImageLoader.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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

  FuzzResourceLoader loader(bytes);
  ImageLoader imageLoader(loader);

  // Use a fixed URL with a raster image extension. stb_image auto-detects the actual format
  // from the file's magic bytes regardless of the extension hint, so this exercises PNG, JPEG,
  // GIF, BMP, and other stb-supported decoders on arbitrary fuzz bytes.
  auto result = imageLoader.fromUri("fuzz-input.png");
  (void)result;

  return 0;
}

}  // namespace donner::svg
