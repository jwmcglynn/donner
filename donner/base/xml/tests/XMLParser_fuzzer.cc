
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

#include "donner/base/xml/XMLParser.h"

namespace donner::xml {

namespace {

int readEnvInt(const char* name, int defaultValue) {
  if (const char* env = std::getenv(name)) {
    const long parsed = std::strtol(env, nullptr, 10);  // NOLINT(runtime/int)
    if (parsed > 0 && parsed <= std::numeric_limits<int>::max()) {
      return static_cast<int>(parsed);
    }
  }

  return defaultValue;
}

uint64_t readEnvUint64(const char* name, uint64_t defaultValue) {
  if (const char* env = std::getenv(name)) {
    const unsigned long long parsed = std::strtoull(env, nullptr, 10);  // NOLINT(runtime/int)
    if (parsed > 0) {
      return static_cast<uint64_t>(parsed);
    }
  }

  return defaultValue;
}

XMLParser::Options applyEntityLimits(XMLParser::Options options) {
  options.maxEntityDepth = readEnvInt("DONNER_XML_FUZZ_MAX_DEPTH", options.maxEntityDepth);
  options.maxEntitySubstitutions =
      readEnvUint64("DONNER_XML_FUZZ_MAX_SUBS", options.maxEntitySubstitutions);
  return options;
}

void logLimitHits(const ParseResult<XMLDocument>& result) {
  if (result.hasError()) {
    const std::string_view reason = result.error().reason;
    if (reason.find("depth exceeded") != std::string::npos) {
      (void)fprintf(stderr, "HIT_DEPTH_CAP\n");
    }
    if (reason.find("substitution limit") != std::string::npos) {
      (void)fprintf(stderr, "HIT_SUBS_CAP\n");
    }
  }
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view str(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                             size);
  // Default parse flags
  logLimitHits(XMLParser::Parse(str, applyEntityLimits(XMLParser::Options())));

  // Full flags
  logLimitHits(XMLParser::Parse(str, applyEntityLimits(XMLParser::Options::ParseAll())));

  // Full flags, no entity translation
  {
    XMLParser::Options options = applyEntityLimits(XMLParser::Options::ParseAll());
    options.disableEntityTranslation = true;

    logLimitHits(XMLParser::Parse(str, options));
  }

  return 0;
}

}  // namespace donner::xml
