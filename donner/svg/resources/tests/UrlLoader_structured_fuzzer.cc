#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/svg/resources/UrlLoader.h"

namespace donner::svg {
namespace {

class PayloadResourceLoader : public ResourceLoaderInterface {
public:
  explicit PayloadResourceLoader(std::vector<uint8_t> payload) : payload_(std::move(payload)) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view /*url*/) override {
    ++fetchCount_;
    return payload_;
  }

  size_t fetchCount() const { return fetchCount_; }

private:
  std::vector<uint8_t> payload_;
  size_t fetchCount_ = 0;
};

std::string PercentEncode(std::span<const uint8_t> data) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string result = "data:application/octet-stream,";
  result.reserve(result.size() + data.size() * 3);
  for (uint8_t byte : data) {
    result.push_back('%');
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0F]);
  }
  return result;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);  // NOLINT
  const bool forceExternalUriLimit = input.starts_with("external-uri-representation-budget");
  const bool forceZeroPerResourceLimit = input.starts_with("zero-per-resource-limit");
  FuzzedDataProvider provider(data, size);
  const size_t maximumResourceSize =
      forceZeroPerResourceLimit ? 0 : provider.ConsumeIntegralInRange<size_t>(0, 128);
  size_t remainingResourceBytes =
      forceZeroPerResourceLimit ? 44 : provider.ConsumeIntegralInRange<size_t>(0, 256);
  const std::vector<uint8_t> payload =
      forceZeroPerResourceLimit
          ? std::vector<uint8_t>{}
          : provider.ConsumeBytes<uint8_t>(provider.ConsumeIntegralInRange<size_t>(
                0, std::min<size_t>(128, provider.remaining_bytes())));

  PayloadResourceLoader resourceLoader(payload);
  UrlLoader urlLoader(resourceLoader, maximumResourceSize, &remainingResourceBytes);
  const std::string externalUrl = "asset/" + provider.ConsumeRandomLengthString(32);
  const std::string dataUrl = PercentEncode(payload);

  if (forceExternalUriLimit) {
    const auto result = urlLoader.fromUri(std::string(UrlLoader::kMaximumExternalUriSize + 1, 'a'));
    if (!std::holds_alternative<UrlLoaderError>(result) ||
        std::get<UrlLoaderError>(result) != UrlLoaderError::ResourceTooLarge ||
        resourceLoader.fetchCount() != 0) {
      std::abort();
    }
  }

  for (std::string_view uri :
       {std::string_view(externalUrl), std::string_view(dataUrl), std::string_view(externalUrl)}) {
    const size_t before = remainingResourceBytes;
    const size_t fetchCountBefore = resourceLoader.fetchCount();
    auto result = urlLoader.fromUri(uri);
    if (const auto* loaded = std::get_if<UrlLoader::Result>(&result)) {
      if (loaded->data.size() > maximumResourceSize || loaded->data.size() > before ||
          remainingResourceBytes != before - loaded->data.size()) {
        std::abort();
      }
    } else {
      const UrlLoaderError error = std::get<UrlLoaderError>(result);
      const bool zeroPerResourceLimitRejectedBeforeFetch =
          error == UrlLoaderError::ResourceTooLarge && maximumResourceSize == 0 &&
          !uri.starts_with("data:") && remainingResourceBytes == before &&
          resourceLoader.fetchCount() == fetchCountBefore;
      if ((error == UrlLoaderError::ResourceTooLarge && remainingResourceBytes != 0 &&
           !zeroPerResourceLimitRejectedBeforeFetch) ||
          (error != UrlLoaderError::ResourceTooLarge && remainingResourceBytes != before)) {
        std::abort();
      }
    }
  }
  return 0;
}

}  // namespace donner::svg
