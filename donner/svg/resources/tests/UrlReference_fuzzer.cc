#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "donner/svg/resources/UrlReference.h"

namespace donner::svg {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html. The input is split at the first
/// newline into a base URL and a reference.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view buffer(reinterpret_cast<const char*>(data), size);
  const size_t newline = buffer.find('\n');
  const std::string_view base = buffer.substr(0, newline);
  const std::string_view reference =
      newline == std::string_view::npos ? std::string_view() : buffer.substr(newline + 1);

  const auto resolved = ResolveUrlReference(base, reference);
  if (resolved.has_value()) {
    // Resolution only adds a few separator and disambiguation bytes to its inputs.
    if (resolved->size() > base.size() + reference.size() + 4) {
      std::abort();
    }
    // A resolved URL is already normalized: resolving it again against the document is stable.
    const auto again = ResolveUrlReference("", *resolved);
    if (!again.has_value() || *again != *resolved) {
      std::abort();
    }
  }
  return 0;
}

}  // namespace donner::svg
