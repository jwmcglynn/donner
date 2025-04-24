/**
 * A *structured* libFuzzer target for donner::xml::XMLParser that generates
 * syntactically-correct XML with random DOCTYPE/entity constructs, attributes,
 * comments, CDATA, processing instructions, and nested elements. The goal is to
 * reach deep paths such as consumeAndExpandEntities() and to validate the
 * mitigation against exponential-growth entity attacks (e.g. "Billion Laughs").
 */

#include <fuzzer/FuzzedDataProvider.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/xml/XMLParser.h"

namespace donner::xml {

namespace {

/// Maximum number of entity declarations to emit.  Kept well below the
/// parser’s kMaxEntityDepth (10) to avoid pathological run-time cost.
static constexpr int kMaxEntities = 8;

/// Valid XML 1.0 name characters, kept small for speed.
constexpr std::string_view kNameAlphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

/// Pick a random XML name (optionally with an ns-prefix) from the fuzzer.
std::string ConsumeName(FuzzedDataProvider& provider, bool withNamespacePrefix) {
  const size_t len = provider.ConsumeIntegralInRange<size_t>(1, 12);
  std::string name = provider.ConsumeBytesAsString(len);
  if (name.empty()) {  // ensure at least one legal char
    name.push_back(
        kNameAlphabet[provider.ConsumeIntegralInRange<size_t>(0, kNameAlphabet.size() - 1)]);
  }
  // Strip illegal chars quickly.
  for (char& c : name) {
    if (static_cast<unsigned char>(c) > 0x7F || c == ':' || c == '-' || c == '.') c = 'a';
  }
  if (withNamespacePrefix) {
    return "ns" + std::to_string(provider.ConsumeIntegral<uint8_t>()) + ":" + name;
  }
  return name;
}

/// Emit a <!ENTITY …> declaration.  Returns the declared name.
std::string EmitEntityDecl(FuzzedDataProvider& provider, std::string& outDoctypeInternalSubset,
                           const std::vector<std::string>& earlierEntities) {
  const bool external = provider.ConsumeBool();
  const std::string entName = ConsumeName(provider, /*withNamespacePrefix=*/false);

  outDoctypeInternalSubset += "<!ENTITY ";
  if (provider.ConsumeBool()) {  // parameter entity?
    outDoctypeInternalSubset.push_back('%');
    outDoctypeInternalSubset.push_back(' ');
  }
  outDoctypeInternalSubset += entName;
  outDoctypeInternalSubset.push_back(' ');

  if (external) {
    // Simple external identifier -- we do *not* dereference it; parser will treat as external and
    // skip expansion.
    outDoctypeInternalSubset += "SYSTEM \"http://example.com/ext\"";
  } else {
    // Build the value. 50% chance of referencing earlier entity to exercise recursion control.
    std::string value;
    if (!earlierEntities.empty() && provider.ConsumeBool()) {
      const std::string& ref =
          earlierEntities[provider.ConsumeIntegralInRange<size_t>(0, earlierEntities.size() - 1)];
      const int repeat = provider.ConsumeIntegralInRange<int>(1, 5);
      for (int i = 0; i < repeat; ++i) {
        value += '&';
        value += ref;
        value += ';';
      }
    } else {
      // Raw text.
      const size_t txtLen = provider.ConsumeIntegralInRange<size_t>(0, 32);
      value = provider.ConsumeBytesAsString(txtLen);
    }

    // Quote choice
    const char quote = provider.ConsumeBool() ? '"' : '\'';
    outDoctypeInternalSubset.push_back(quote);
    outDoctypeInternalSubset += value;
    outDoctypeInternalSubset.push_back(quote);
  }

  outDoctypeInternalSubset += ">";
  return entName;
}

/// Generate a random attribute string " name="value"" (leading space included).
void EmitAttribute(FuzzedDataProvider& provider, std::string& out) {
  out.push_back(' ');
  const std::string name = ConsumeName(provider, /*withNamespacePrefix=*/provider.ConsumeBool());
  out += name;
  out.push_back('=');
  const char quote = provider.ConsumeBool() ? '"' : '\'';
  out.push_back(quote);

  const bool useEntityRef = provider.ConsumeBool();
  if (useEntityRef) {
    out.push_back('&');
    out += ConsumeName(provider, /*ns*/ false);
    out.push_back(';');
  } else {
    out += provider.ConsumeRandomLengthString(16);
  }
  out.push_back(quote);
}

/// Assemble a complete XML document string.
std::string BuildXmlString(FuzzedDataProvider& provider) {
  std::string xml;

  // 1. Optional BOM
  if (provider.ConsumeBool()) {
    xml.append("\xEF\xBB\xBF");  // UTF-8 BOM
  }

  // 2. Optional XML declaration
  if (provider.ConsumeBool()) {
    xml.append("<?xml");
    if (provider.ConsumeBool()) xml.append(" version=\"1.0\"");
    if (provider.ConsumeBool()) xml.append(" encoding=\"UTF-8\"");
    if (provider.ConsumeBool()) xml.append(" standalone=\"yes\"");
    xml.append("?>");
  }

  // 3. Optional DOCTYPE + entity declarations
  std::vector<std::string> declaredEntities;
  if (provider.ConsumeBool()) {
    xml.append("<!DOCTYPE ");
    const std::string rootName = ConsumeName(provider, /*ns*/ false);
    xml += rootName;

    std::string internalSubset;
    const int numEntities = provider.ConsumeIntegralInRange<int>(0, kMaxEntities);
    for (int i = 0; i < numEntities; ++i) {
      declaredEntities.emplace_back(EmitEntityDecl(provider, internalSubset, declaredEntities));
    }
    if (!internalSubset.empty()) {
      xml.append(" [");
      xml.append(internalSubset);
      xml.push_back(']');
    }
    xml.append(">");
  }

  // 4. Root element
  const std::string rootTag = provider.ConsumeBool() ? "svg" : ConsumeName(provider, false);
  xml.push_back('<');
  xml += rootTag;

  // Random attributes on root
  const int numRootAttrs = provider.ConsumeIntegralInRange<int>(0, 8);
  for (int i = 0; i < numRootAttrs; ++i) EmitAttribute(provider, xml);

  const bool selfClosingRoot = provider.ConsumeBool();
  if (selfClosingRoot) {
    xml.append("/>");
    return xml;
  } else {
    xml.push_back('>');
  }

  // 5. Child contents
  const int numChildren = provider.ConsumeIntegralInRange<int>(0, 20);
  for (int i = 0; i < numChildren; ++i) {
    switch (provider.ConsumeIntegralInRange<int>(0, 5)) {
      case 0: {  // Nested element
        xml.push_back('<');
        const std::string tag = ConsumeName(provider, provider.ConsumeBool());
        xml += tag;
        const int numAttrs = provider.ConsumeIntegralInRange<int>(0, 4);
        for (int j = 0; j < numAttrs; ++j) EmitAttribute(provider, xml);
        xml.push_back('>');

        // Optionally reference an entity inside
        if (!declaredEntities.empty() && provider.ConsumeBool()) {
          xml.push_back('&');
          xml += declaredEntities[provider.ConsumeIntegralInRange<size_t>(
              0, declaredEntities.size() - 1)];
          xml.push_back(';');
        }

        xml.append("</");
        xml += tag;
        xml.push_back('>');
        break;
      }
      case 1:  // Character data
        xml.append(provider.ConsumeRandomLengthString(32));
        break;
      case 2:  // CDATA section
        xml.append("<![CDATA[");
        xml.append(provider.ConsumeRandomLengthString(32));
        xml.append("]]>");
        break;
      case 3:  // Comment
        xml.append("<!--");
        xml.append(provider.ConsumeRandomLengthString(32));
        xml.append("-->");
        break;
      case 4:  // PI
        xml.append("<?");
        xml.append(ConsumeName(provider, false));
        xml.push_back(' ');
        xml.append(provider.ConsumeRandomLengthString(32));
        xml.append("?>");
        break;
      case 5:  // Entity reference only
        if (!declaredEntities.empty()) {
          xml.push_back('&');
          xml += declaredEntities[provider.ConsumeIntegralInRange<size_t>(
              0, declaredEntities.size() - 1)];
          xml.push_back(';');
        }
        break;
    }
  }

  // Close root
  xml.append("</");
  xml += rootTag;
  xml.push_back('>');

  return xml;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider provider(data, size);

  // 1. Construct a structured XML payload.
  const std::string xml = BuildXmlString(provider);

  if (::getenv("DUMP")) {
    // Print the generated XML for debugging purposes.
    std::cout << "---------------\n";
    std::cout << xml << "\n";
    std::cout << "---------------\n";
  }

  // 2. Exercise the parser under several configurations to maximise
  //    coverage of optional code paths.
  using XMLParserOptions = donner::xml::XMLParser::Options;

  (void)donner::xml::XMLParser::Parse(xml);  // default

  (void)donner::xml::XMLParser::Parse(xml, XMLParserOptions::ParseAll());  // everything enabled

  XMLParserOptions opts = XMLParserOptions::ParseAll();
  opts.disableEntityTranslation = true;  // no entity expansion
  (void)donner::xml::XMLParser::Parse(xml, opts);

  return 0;
}

}  // namespace donner::xml
