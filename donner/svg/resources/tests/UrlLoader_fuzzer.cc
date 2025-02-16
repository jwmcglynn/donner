#include "donner/svg/resources/UrlLoader.h"

namespace donner::svg::parser {

/// An no-op resource loader.
class NoOpResourceLoader : public ResourceLoaderInterface {
public:
  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    return ResourceLoaderError::NotFound;
  }
};

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view buffer(reinterpret_cast<const char*>(data), size);

  NoOpResourceLoader loader;
  UrlLoader urlLoader(loader);

  auto result = urlLoader.fromUri(buffer);
  (void)result;

  return 0;
}

}  // namespace donner::svg::parser
