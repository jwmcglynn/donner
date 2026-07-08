/**
 * A libFuzzer target for donner::xml::XMLIncrementalParser, the entry points used by
 * structured editing (see XMLDocument::applySourceEdit). Each entry point wraps a raw
 * source-edit fragment with synthetic markup before delegating to XMLParser::Parse, so the
 * wrapping logic itself (string concatenation, the ParseOpeningTag self-closing-tag
 * rewrite) is exercised here in addition to the underlying parser. These fragments
 * originate from editor-side source edits and must be treated as untrusted: malformed or
 * adversarial fragments must be rejected cleanly, never crash or read out of bounds.
 */

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "donner/base/xml/XMLIncrementalParser.h"

namespace donner::xml {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }

  // First byte selects which incremental-parse entry point to exercise, the rest of the
  // bytes are the raw source-edit fragment.
  const uint8_t selector = data[0];
  const std::string_view fragment(reinterpret_cast<const char*>(data + 1),  // NOLINT
                                  size - 1);

  switch (selector % 5) {
    case 0: (void)XMLIncrementalParser::ParseAttribute(fragment); break;
    case 1: (void)XMLIncrementalParser::ParseOpeningTag(fragment); break;
    case 2: (void)XMLIncrementalParser::ParsePcdata(fragment); break;
    case 3: (void)XMLIncrementalParser::ParseTextLikeNode(fragment); break;
    case 4: (void)XMLIncrementalParser::ParseElement(fragment); break;
  }

  return 0;
}

}  // namespace donner::xml
