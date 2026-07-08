#include <cassert>
#include <string>

#include "donner/base/Utf8.h"

namespace donner {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
///
/// Exercises UTF-8 decoding, which runs directly over untrusted SVG/CSS document text (see
/// donner/css/parser/details/Tokenizer.cc and donner/base/xml/XMLParser.cc).
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view buffer(reinterpret_cast<const char*>(data), size);

  // Decode the entire buffer with both the strict and lenient decoders, verifying that the
  // consumed byte count always makes forward progress and never overruns the buffer.
  {
    std::string_view remaining = buffer;
    while (!remaining.empty()) {
      const auto [codepoint, consumed] = Utf8::NextCodepoint(remaining);
      assert(consumed >= 0 && "consumed length should be non-negative");
      assert(static_cast<size_t>(consumed) <= remaining.size() &&
             "consumed length should not exceed remaining buffer size");
      if (consumed <= 0) {
        break;
      }

      if (Utf8::IsValidCodepoint(codepoint)) {
        // Round-trip through Append and verify it does not crash.
        std::string encoded;
        Utf8::Append(codepoint, std::back_inserter(encoded));
        (void)encoded;
      }

      remaining.remove_prefix(static_cast<size_t>(consumed));
    }
  }

  {
    std::string_view remaining = buffer;
    while (!remaining.empty()) {
      const auto [codepoint, consumed] = Utf8::NextCodepointLenient(remaining);
      (void)codepoint;
      assert(consumed >= 0 && "consumed length should be non-negative");
      assert(static_cast<size_t>(consumed) <= remaining.size() &&
             "consumed length should not exceed remaining buffer size");
      if (consumed <= 0) {
        break;
      }
      remaining.remove_prefix(static_cast<size_t>(consumed));
    }
  }

  // SequenceLength should never crash for any byte value.
  if (!buffer.empty()) {
    const int len = Utf8::SequenceLength(buffer[0]);
    assert(len >= 0 && len <= 4 && "SequenceLength should return 0-4");
  }

  return 0;
}

}  // namespace donner
