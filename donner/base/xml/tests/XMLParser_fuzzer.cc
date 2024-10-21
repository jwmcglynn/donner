
#include "donner/base/xml/XMLParser.h"

namespace donner::xml {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view str(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                             size);
  // Default parse flags
  (void)XMLParser::Parse(str);

  // Full flags
  (void)XMLParser::Parse(str, XMLParser::Options::ParseAll());

  // Full flags, no entity translation
  {
    XMLParser::Options options = XMLParser::Options::ParseAll();
    options.disableEntityTranslation = true;

    (void)XMLParser::Parse(str, options);
  }

  return 0;
}

}  // namespace donner::xml
