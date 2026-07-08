/**
 * A libFuzzer target for donner::xml::EscapeAttributeValue and donner::xml::EscapeTextContent.
 * Both functions hand-roll UTF-8 validation (sequence length, continuation-byte checks,
 * overlong-encoding rejection, surrogate/non-character rejection) over arbitrary bytes, which
 * makes them a good target for out-of-bounds reads on truncated multi-byte sequences and other
 * decoder edge cases. The values passed to these functions typically originate from document
 * content (e.g. round-tripping an edited attribute or text node back to source), so the input
 * must be treated as untrusted.
 */

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "donner/base/xml/XMLEscape.h"

namespace donner::xml {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view value(reinterpret_cast<const char*>(data),  // NOLINT
                               size);

  (void)EscapeAttributeValue(value, '"');
  (void)EscapeAttributeValue(value, '\'');
  (void)EscapeTextContent(value);

  return 0;
}

}  // namespace donner::xml
