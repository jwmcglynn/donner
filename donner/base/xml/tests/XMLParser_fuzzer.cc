
#include <cstdint>
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
    const uint64_t parsed = static_cast<uint64_t>(std::strtoull(env, nullptr, 10));
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
    if (reason.find("Maximum element count") != std::string::npos) {
      (void)fprintf(stderr, "HIT_ELEMENTS_CAP\n");
    }
    if (reason.find("attributes-per-element") != std::string::npos) {
      (void)fprintf(stderr, "HIT_ATTRS_CAP\n");
    }
    if (reason.find("nesting depth") != std::string::npos) {
      (void)fprintf(stderr, "HIT_NESTING_CAP\n");
    }
  }
}

/// Drive `GetAttributeLocation` with an arbitrary offset + attribute name
/// derived from the fuzz input. This exercises the "editor calls with a
/// stale offset" path — historically the inner helper was gated by
/// `UTILS_RELEASE_ASSERT`s that would abort on malformed input; now it
/// must return `std::nullopt` cleanly and cannot crash or read out of
/// bounds regardless of the offset we pass.
void fuzzGetAttributeLocation(std::string_view source, const uint8_t* data, size_t size) {
  if (source.empty() || size < 2) {
    return;
  }

  // Derive an arbitrary offset from the first byte (wrapped into [0, size]
  // range — the function must accept out-of-range offsets too, but we
  // want most runs to land somewhere plausible).
  const std::size_t rawOffset = data[0];
  const std::size_t offset = rawOffset % (source.size() + 1);
  const FileOffset fileOffset = FileOffset::Offset(offset);

  // Second byte → attribute-name length. Third+ bytes → name payload.
  const std::size_t nameLen =
      std::min<std::size_t>(data[1] % 16, size > 2 ? size - 2 : 0);
  const std::string_view nameBytes(reinterpret_cast<const char*>(data + 2), nameLen);
  // Constrain to an ASCII-ish subset so we occasionally match a real
  // attribute name without biasing away from random garbage.
  std::string name;
  name.reserve(nameLen);
  for (char c : nameBytes) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c == ':') {
      name.push_back(c);
    }
  }
  if (name.empty()) {
    name = "fill";  // Keep a default so the function exercises the hit path sometimes.
  }

  const XMLQualifiedNameRef attrName(name);
  // Result deliberately ignored — the contract is "no crash, no OOB
  // read" regardless of the input shape.
  (void)XMLParser::GetAttributeLocation(source, fileOffset, attrName);

  // Also probe the pathological out-of-range case so the fuzzer reliably
  // hits the bounds check in GetAttributeLocation.
  (void)XMLParser::GetAttributeLocation(source,
                                        FileOffset::Offset(source.size() + 1), attrName);
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

  // Exercise GetAttributeLocation — M−1 prerequisite for structured
  // editing. The inner helper previously release-asserted on malformed
  // input; this arm of the fuzzer is the regression net for that fix.
  fuzzGetAttributeLocation(str, data, size);

  return 0;
}

}  // namespace donner::xml
