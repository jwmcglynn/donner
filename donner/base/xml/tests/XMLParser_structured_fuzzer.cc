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
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/parser/LineOffsets.h"
#include "donner/base/xml/XMLParser.h"

namespace donner::xml {

namespace {

/// Maximum number of entity declarations to emit.  Kept well below the
/// parser's kMaxEntityDepth (10) to avoid pathological run-time cost.
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

/// Emit a <!ENTITY ...> declaration.  Returns the declared name.
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

void ExerciseAggregateLimitMarkers(std::string_view input) {
  if (input.starts_with("element-lifecycle-budget")) {
    XMLParser::Options options;
    options.maxElements = 128;
    std::string xml = "<root>";
    for (std::size_t i = 0; i < options.maxElements; ++i) {
      xml += "<g/>";
    }
    xml += "</root>";
    const auto result = XMLParser::Parse(xml, options);
    if (result.hasResult() ||
        result.error().reason.str().find("element") == std::string_view::npos) {
      std::abort();
    }
  } else if (input.starts_with("data-node-budget")) {
    XMLParser::Options options;
    options.parseComments = false;
    options.maxElements = 128;
    std::string xml = "<root>";
    for (std::size_t i = 0; i < options.maxElements; ++i) {
      xml += "x<!---->";
    }
    xml += "</root>";
    const auto result = XMLParser::Parse(xml, options);
    if (result.hasResult() ||
        result.error().reason.str().find("element count") == std::string_view::npos) {
      std::abort();
    }
  } else if (input.starts_with("total-attribute-budget")) {
    XMLParser::Options options;
    options.maxAttributesPerElement = 16;
    options.maxTotalAttributes = 16;
    const auto result = XMLParser::Parse(
        "<root><a a0='0' a1='1' a2='2' a3='3' a4='4' a5='5' a6='6' a7='7' a8='8'/><b "
        "b0='0' b1='1' b2='2' b3='3' b4='4' b5='5' b6='6' b7='7'/></root>",
        options);
    if (result.hasResult() ||
        result.error().reason.str().find("total attribute") == std::string_view::npos) {
      std::abort();
    }
  } else if (input.starts_with("entity-declaration-count-budget")) {
    XMLParser::Options options = XMLParser::Options::ParseAll();
    options.maxEntityDeclarations = 4;
    const auto result = XMLParser::Parse(
        "<!DOCTYPE root [<!ENTITY a 'a'><!ENTITY b 'b'><!ENTITY c 'c'><!ENTITY d 'd'>"
        "<!ENTITY e 'e'>]><root/>",
        options);
    if (result.hasResult() ||
        result.error().reason.str().find("entity declaration") == std::string_view::npos) {
      std::abort();
    }
  } else if (input.starts_with("entity-declaration-byte-budget")) {
    XMLParser::Options options = XMLParser::Options::ParseAll();
    options.maxEntityDeclarationBytes = 8;
    const auto result =
        XMLParser::Parse("<!DOCTYPE root [<!ENTITY longname 'longvalue'>]><root/>", options);
    if (result.hasResult() ||
        result.error().reason.str().find("entity declaration") == std::string_view::npos) {
      std::abort();
    }
  } else if (input.starts_with("invalid-xml-null-entity")) {
    const auto result = XMLParser::Parse("<svg href='..&#0;ignored/secret.bin'/>");
    if (result.hasResult() ||
        result.error().reason.str().find("Invalid numeric character") == std::string_view::npos) {
      std::abort();
    }
  } else if (input.starts_with("invalid-xml-control-entity")) {
    const auto result = XMLParser::Parse("<svg href='safe/&#11;ignored.bin'/>");
    if (result.hasResult() ||
        result.error().reason.str().find("Invalid numeric character") == std::string_view::npos) {
      std::abort();
    }
  } else if (input.starts_with("newline-index-budget")) {
    std::string xml = "<root>";
    xml.append(parser::LineOffsets::kMaximumStoredLineOffsets + 1, '\n');
    for (int attribute = 0; attribute < 64; ++attribute) {
      xml += "<g a='v'/>\n";
    }
    xml += "</root>";

    parser::LineOffsets offsets(xml, 1024);
    if (!offsets.truncated() || offsets.offsets().size() != 1024) {
      std::abort();
    }
    if (!XMLParser::Parse(xml).hasResult()) {
      std::abort();
    }
  } else if (input.starts_with("incremental-source-size-budget")) {
    XMLParser::Options options;
    options.maximumInputSize = 64;
    auto parsed = XMLParser::Parse("<root/>", options);
    if (!parsed.hasResult()) {
      std::abort();
    }
    XMLDocument document = std::move(parsed.result());

    const std::string nearLimit = "<root>" + std::string(50, 'a') + "</root>";
    const auto accepted = document.applySourceEdit(XMLEditIntent{
        .range = SourceRange{FileOffset::Offset(0), FileOffset::Offset(document.source().size())},
        .replacement = nearLimit,
        .sourceVersion = document.sourceVersion(),
    });
    if (!accepted.applied || document.source() != nearLimit) {
      std::abort();
    }

    const std::uint64_t version = document.sourceVersion();
    const std::string oversized = "<root>" + std::string(52, 'a') + "</root>";
    const auto rejected = document.applySourceEdit(XMLEditIntent{
        .range = SourceRange{FileOffset::Offset(0), FileOffset::Offset(document.source().size())},
        .replacement = oversized,
        .sourceVersion = version,
    });
    if (rejected.applied || document.source() != nearLimit || document.sourceVersion() != version) {
      std::abort();
    }
  } else if (input.starts_with("source-anchor-history-budget")) {
    constexpr std::size_t kChildCount = 64;
    constexpr std::size_t kEditCount = 16;
    std::string children;
    for (std::size_t index = 0; index < kChildCount; ++index) {
      children += "<item id='" + std::to_string(index) + "'/>";
    }

    XMLParser::Options options;
    options.maxElements = kChildCount + 2;
    auto parsed = XMLParser::Parse("<root>" + children + "</root>", options);
    if (!parsed.hasResult()) {
      std::abort();
    }
    XMLDocument document = std::move(parsed.result());
    XMLSourceStore* store = document.sourceStore();
    if (store == nullptr) {
      std::abort();
    }

    const std::size_t editStart = document.source().find("<item");
    const std::size_t editEnd = document.source().find("</root>");
    const XMLSourceStore::ResourceStats initialStats = store->resourceStats();
    if (editStart == std::string_view::npos || editEnd == std::string_view::npos ||
        initialStats.liveAnchorCount == 0) {
      std::abort();
    }
    XMLSourceStore::ResourceLimits limits = store->resourceLimits();
    limits.maximumAnchorUpdateWorkPerEdit = initialStats.liveAnchorCount;
    if (!store->setResourceLimits(limits)) {
      std::abort();
    }

    for (std::size_t edit = 0; edit < kEditCount; ++edit) {
      const auto result = document.applySourceEdit(XMLEditIntent{
          .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
          .replacement = children,
          .sourceVersion = document.sourceVersion(),
      });
      if (!result.applied || result.scope != ReparseScope::ElementSubtree ||
          result.diagnostic.has_value() ||
          store->resourceStats().liveAnchorCount != initialStats.liveAnchorCount) {
        std::abort();
      }
    }

    const XMLSourceStore::ResourceStats repeatedStats = store->resourceStats();
    if (repeatedStats.liveAnchorCount != initialStats.liveAnchorCount ||
        repeatedStats.peakLiveAnchorCount > initialStats.liveAnchorCount + 4 ||
        repeatedStats.totalCreatedAnchors <= initialStats.totalCreatedAnchors ||
        repeatedStats.totalRetiredAnchors <= initialStats.totalRetiredAnchors ||
        repeatedStats.lastAnchorUpdateWork != initialStats.liveAnchorCount ||
        repeatedStats.totalAnchorUpdateWork !=
            initialStats.totalAnchorUpdateWork + kEditCount * initialStats.liveAnchorCount) {
      std::abort();
    }

    XMLNode root = document.root().firstChild().value();
    std::vector<XMLNode> childrenBeforeRejection;
    for (std::optional<XMLNode> child = root.firstChild(); child.has_value();
         child = child->nextSibling()) {
      childrenBeforeRejection.push_back(*child);
    }
    if (childrenBeforeRejection.size() != kChildCount) {
      std::abort();
    }
    limits.maximumAnchorUpdateWorkPerEdit = initialStats.liveAnchorCount - 1;
    if (!store->setResourceLimits(limits)) {
      std::abort();
    }
    const std::string sourceBeforeRejection(document.source());
    const std::string treeBeforeRejection(
        std::string_view(root.serializeToString(0, /*prettyPrint=*/false)));
    const std::uint64_t versionBeforeRejection = document.sourceVersion();

    const auto rejected = document.applySourceEdit(XMLEditIntent{
        .range = SourceRange{FileOffset::Offset(editStart), FileOffset::Offset(editEnd)},
        .replacement = children,
        .sourceVersion = document.sourceVersion(),
    });
    std::vector<XMLNode> childrenAfterRejection;
    for (std::optional<XMLNode> child = root.firstChild(); child.has_value();
         child = child->nextSibling()) {
      childrenAfterRejection.push_back(*child);
    }
    const XMLSourceStore::ResourceStats rejectedStats = store->resourceStats();
    if (rejected.applied || document.source() != sourceBeforeRejection ||
        root.serializeToString(0, /*prettyPrint=*/false) != treeBeforeRejection ||
        document.sourceVersion() != versionBeforeRejection ||
        childrenAfterRejection != childrenBeforeRejection ||
        rejectedStats.liveAnchorCount != repeatedStats.liveAnchorCount ||
        rejectedStats.totalCreatedAnchors != repeatedStats.totalCreatedAnchors ||
        rejectedStats.totalRetiredAnchors != repeatedStats.totalRetiredAnchors ||
        rejectedStats.totalAnchorUpdateWork != repeatedStats.totalAnchorUpdateWork ||
        rejectedStats.lastAnchorUpdateWork != 0 ||
        rejectedStats.anchorUpdateWorkRejections != repeatedStats.anchorUpdateWorkRejections + 1) {
      std::abort();
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  ExerciseAggregateLimitMarkers(
      std::string_view(reinterpret_cast<const char*>(data), size));  // NOLINT
  FuzzedDataProvider provider(data, size);

  // 1. Construct a structured XML payload.
  const std::string xml = BuildXmlString(provider);

  XMLParser::Options limitedOptions;
  limitedOptions.maximumInputSize = xml.size() / 2;
  auto limitedResult = XMLParser::Parse(xml, limitedOptions);
  if (xml.size() > limitedOptions.maximumInputSize && limitedResult.hasResult()) {
    std::abort();
  }

  if (std::getenv("DUMP")) {
    // Print the generated XML for debugging purposes.
    std::cout << "---------------\n";
    std::cout << "\"";
    for (char c : xml) {
      unsigned char uc = static_cast<unsigned char>(c);
      if (c == '\n')
        std::cout << "\\n";
      else if (c == '\r')
        std::cout << "\\r";
      else if (c == '\t')
        std::cout << "\\t";
      else if (c == '\b')
        std::cout << "\\b";
      else if (c == '\f')
        std::cout << "\\f";
      else if (c == '\\')
        std::cout << "\\\\";
      else if (c == '\"')
        std::cout << "\\\"";
      else if (!std::isprint(uc))
        std::cout << "\\x" << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(uc);
      else
        std::cout << c;
    }
    std::cout << "\"\n";
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
