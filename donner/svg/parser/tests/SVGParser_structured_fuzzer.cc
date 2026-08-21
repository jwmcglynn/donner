#include <fuzzer/FuzzedDataProvider.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/svg/SVGElementNames.h"
#include "donner/svg/components/DescriptiveTextComponent.h"
#include "donner/svg/components/GeometryPreparationResourceBudget.h"
#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererUtils.h"

using namespace donner::xml;

namespace {

// Function to create a qualified name, possibly with a namespace
XMLQualifiedName CreateQualifiedName(FuzzedDataProvider& provider) {
  const bool useNamespace = provider.ConsumeBool();
  if (useNamespace) {
    // Generate with a random prefix
    const std::string prefix = provider.ConsumeRandomLengthString(10);
    const std::string localName = provider.ConsumeRandomLengthString(32);
    return XMLQualifiedName(donner::RcString(prefix), donner::RcString(localName));
  } else {
    // Generate without a prefix
    const std::string localName = provider.ConsumeRandomLengthString(32);
    return XMLQualifiedName(donner::RcString(localName));
  }
}

// Function to create a random element name, possibly from known SVG elements
XMLQualifiedName CreateRandomElementName(FuzzedDataProvider& provider) {
  // Either pick from a known element name or generate a random one
  const bool useKnownElementName = provider.ConsumeBool();
  if (useKnownElementName) {
    const std::string_view elementName = provider.PickValueInArray(donner::svg::kSVGElementNames);
    return XMLQualifiedName(donner::RcString(elementName));
  }

  return CreateQualifiedName(provider);
}

// Function to create a random attribute name, possibly from known attributes
XMLQualifiedName CreateRandomAttributeName(FuzzedDataProvider& provider) {
  // Either pick from a known attribute name or generate a random one
  const bool useKnownAttributeName = provider.ConsumeBool();
  if (useKnownAttributeName) {
    const std::string_view attrName =
        provider.PickValueInArray(donner::svg::kSVGPresentationAttributeNames);
    return XMLQualifiedName(donner::RcString(attrName));
  } else {
    // Generate a random attribute name
    return CreateQualifiedName(provider);
  }
}

void AddEdgeCaseSizingAttributes(XMLNode& svgElement, FuzzedDataProvider& provider) {
  constexpr std::string_view kEdgeNumbers[] = {
      "0", "-1", "1e-300", "1e300", "8192", "2147483648", "4294967296", "inf", "nan",
  };

  if (provider.ConsumeBool()) {
    svgElement.setAttribute("width", provider.PickValueInArray(kEdgeNumbers));
    svgElement.setAttribute("height", provider.PickValueInArray(kEdgeNumbers));
  }
  if (provider.ConsumeBool()) {
    const std::string viewBox = "0 0 " + std::string(provider.PickValueInArray(kEdgeNumbers)) +
                                " " + std::string(provider.PickValueInArray(kEdgeNumbers));
    svgElement.setAttribute("viewBox", viewBox);
  }
}

void AddTextWithFontSize(XMLDocument& document, XMLNode& svgElement, std::string_view fontSize) {
  XMLNode text = XMLNode::CreateElementNode(document, "text");
  text.setAttribute("style", "font-size:" + std::string(fontSize) + "px");
  text.appendChild(XMLNode::CreateDataNode(document, "A"));
  svgElement.appendChild(text);
}

void AddDistributedAttributePayload(XMLDocument& document, XMLNode& svgElement) {
  std::string list = "0";
  for (std::size_t i = 1; i < 64; ++i) {
    list.append(" 0");
  }
  for (std::size_t i = 0; i < 32; ++i) {
    XMLNode text = XMLNode::CreateElementNode(document, "text");
    text.setAttribute("x", list);
    svgElement.appendChild(text);
  }
}

void AddDistributedStylesheets(XMLDocument& document, XMLNode& svgElement) {
  std::string css;
  for (std::size_t i = 0; i < 16; ++i) {
    css.append("rect{stroke-dasharray:0 0 0 0}");
  }
  for (std::size_t i = 0; i < 16; ++i) {
    XMLNode style = XMLNode::CreateElementNode(document, "style");
    style.appendChild(XMLNode::CreateDataNode(document, std::string_view(css)));
    svgElement.appendChild(style);
  }
}

void AddDistributedDeferredPaths(XMLDocument& document, XMLNode& svgElement) {
  std::string pathData = "M0 0";
  for (std::size_t i = 0; i < 128; ++i) {
    pathData.append("H0");
  }
  for (std::size_t i = 0; i < 32; ++i) {
    XMLNode path = XMLNode::CreateElementNode(document, "path");
    path.setAttribute("d", pathData);
    svgElement.appendChild(path);
  }
}

void AddOversizedInlineStyle(XMLDocument& document, XMLNode& svgElement) {
  std::string style = "fill:";
  for (std::size_t i = 0; i <= donner::css::parser::DeclarationListParser::kMaximumComponentValues;
       ++i) {
    style.append("0 ");
  }
  XMLNode rect = XMLNode::CreateElementNode(document, "rect");
  rect.setAttribute("id", "inline-style-component-budget");
  rect.setAttribute("style", style);
  svgElement.appendChild(rect);
}

void AddChunkedProjection(XMLDocument& document, XMLNode& svgElement, std::string_view tag,
                          std::string_view id, std::string_view value) {
  XMLNode element = XMLNode::CreateElementNode(document, tag);
  element.setAttribute("id", id);
  for (std::size_t i = 0; i < 33; ++i) {
    if (i % 2 == 0) {
      element.appendChild(XMLNode::CreateDataNode(document, value));
    } else {
      element.appendChild(XMLNode::CreateCDataNode(document, value));
    }
  }
  svgElement.appendChild(element);
}

void AddEdgeCaseText(XMLDocument& document, XMLNode& svgElement, FuzzedDataProvider& provider) {
  constexpr std::string_view kFontSizes[] = {
      "0", "-1", "1e-300", "1e999", "-1e999", "2147483648", "4294967296",
  };
  AddTextWithFontSize(document, svgElement, provider.PickValueInArray(kFontSizes));
}

// Function to build the XML tree iteratively
void BuildXMLTree(XMLDocument& document, XMLNode& root, FuzzedDataProvider& provider) {
  // Use a vector as a stack of nodes to process
  std::vector<XMLNode> nodesToProcess;
  nodesToProcess.push_back(root);  // Start with the root node

  while (!nodesToProcess.empty()) {
    // Pop a node from the stack
    XMLNode parent = nodesToProcess.back();
    nodesToProcess.pop_back();

    // Decide how many children to create (0 to 5 for each node)
    const size_t numChildren = provider.ConsumeIntegralInRange<size_t>(0, 5);

    for (size_t i = 0; i < numChildren; ++i) {
      // Decide node type
      using NodeType = XMLNode::Type;
      constexpr NodeType possibleNodeTypes[] = {
          NodeType::Element,       NodeType::Data,    NodeType::CData,
          NodeType::Comment,       NodeType::DocType, NodeType::ProcessingInstruction,
          NodeType::XMLDeclaration};

      const NodeType nodeType = provider.PickValueInArray(possibleNodeTypes);

      std::optional<XMLNode> childNode;

      switch (nodeType) {
        case NodeType::Element: {
          // Create an element node
          const XMLQualifiedName tagName = CreateRandomElementName(provider);
          childNode = XMLNode::CreateElementNode(document, tagName);

          // Add an arbitrary number of attributes (0 to 100)
          const size_t numAttributes = provider.ConsumeIntegralInRange<size_t>(0, 100);
          for (size_t j = 0; j < numAttributes; ++j) {
            const XMLQualifiedName attrName = CreateRandomAttributeName(provider);
            const std::string attrValue = provider.ConsumeRandomLengthString(20);
            childNode->setAttribute(attrName, attrValue);
          }

          // Add the child node to the stack to process its children later
          nodesToProcess.push_back(childNode.value());
          break;
        }
        case NodeType::Data: {
          // Create a data node
          const std::string value = provider.ConsumeRandomLengthString(50);
          childNode = XMLNode::CreateDataNode(document, std::string_view(value));
          break;
        }
        case NodeType::CData: {
          // Create a CDATA node
          const std::string value = provider.ConsumeRandomLengthString(50);
          childNode = XMLNode::CreateCDataNode(document, std::string_view(value));
          break;
        }
        case NodeType::Comment: {
          // Create a comment node
          const std::string value = provider.ConsumeRandomLengthString(50);
          childNode = XMLNode::CreateCommentNode(document, std::string_view(value));
          break;
        }
        case NodeType::DocType: {
          // Create a DocType node
          const std::string value = provider.ConsumeRandomLengthString(50);
          childNode = XMLNode::CreateDocTypeNode(document, std::string_view(value));
          break;
        }
        case NodeType::ProcessingInstruction: {
          // Create a ProcessingInstruction node
          std::string target = provider.ConsumeRandomLengthString(10);
          if (target.empty()) {
            target = "pi";  // Default target if empty
          }
          const std::string value = provider.ConsumeRandomLengthString(50);
          childNode = XMLNode::CreateProcessingInstructionNode(document, std::string_view(target),
                                                               std::string_view(value));
          break;
        }
        case NodeType::XMLDeclaration: {
          // Create an XMLDeclaration node
          childNode = XMLNode::CreateXMLDeclarationNode(document);

          // Optionally set attributes (0 to 10)
          const size_t numAttributes = provider.ConsumeIntegralInRange<size_t>(0, 10);
          for (size_t j = 0; j < numAttributes; ++j) {
            const XMLQualifiedName attrName = CreateRandomAttributeName(provider);
            const std::string attrValue = provider.ConsumeRandomLengthString(128);
            childNode->setAttribute(attrName, attrValue);
          }
          break;
        }
        default:
          // Should not reach here
          UTILS_UNREACHABLE();
          continue;
      }

      // Append the child node to the parent
      parent.appendChild(childNode.value());
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  constexpr std::string_view kNonFiniteFontSizeSeed = "NONFINITE_FONT_SIZE\n";
  constexpr std::string_view kParsedPayloadBudgetSeed = "PARSED_PAYLOAD_BUDGET\n";
  constexpr std::string_view kStylesheetAggregateBudgetSeed = "STYLESHEET_AGGREGATE_BUDGET\n";
  constexpr std::string_view kDeferredPathPayloadBudgetSeed = "DEFERRED_PATH_PAYLOAD_BUDGET\n";
  constexpr std::string_view kChunkedTextProjectionSeed = "CHUNKED_TEXT_PROJECTION\n";
  constexpr std::string_view kChunkedStyleProjectionSeed = "CHUNKED_STYLE_PROJECTION\n";
  constexpr std::string_view kChunkedMetadataProjectionSeed = "CHUNKED_METADATA_PROJECTION\n";
  constexpr std::string_view kXmlDataNodeBudgetSeed = "XML_DATA_NODE_BUDGET\n";
  constexpr std::string_view kInlineStyleComponentBudgetSeed = "INLINE_STYLE_COMPONENT_BUDGET\n";
  const bool forceNonFiniteFontSize = size == kNonFiniteFontSizeSeed.size() &&
                                      std::memcmp(data, kNonFiniteFontSizeSeed.data(), size) == 0;
  const bool forceParsedPayloadBudget =
      size == kParsedPayloadBudgetSeed.size() &&
      std::memcmp(data, kParsedPayloadBudgetSeed.data(), size) == 0;
  const bool forceStylesheetAggregateBudget =
      size == kStylesheetAggregateBudgetSeed.size() &&
      std::memcmp(data, kStylesheetAggregateBudgetSeed.data(), size) == 0;
  const bool forceDeferredPathPayloadBudget =
      size == kDeferredPathPayloadBudgetSeed.size() &&
      std::memcmp(data, kDeferredPathPayloadBudgetSeed.data(), size) == 0;
  const bool forceChunkedTextProjection =
      size == kChunkedTextProjectionSeed.size() &&
      std::memcmp(data, kChunkedTextProjectionSeed.data(), size) == 0;
  const bool forceChunkedStyleProjection =
      size == kChunkedStyleProjectionSeed.size() &&
      std::memcmp(data, kChunkedStyleProjectionSeed.data(), size) == 0;
  const bool forceChunkedMetadataProjection =
      size == kChunkedMetadataProjectionSeed.size() &&
      std::memcmp(data, kChunkedMetadataProjectionSeed.data(), size) == 0;
  const bool forceXmlDataNodeBudget = size == kXmlDataNodeBudgetSeed.size() &&
                                      std::memcmp(data, kXmlDataNodeBudgetSeed.data(), size) == 0;
  const bool forceInlineStyleComponentBudget =
      size == kInlineStyleComponentBudgetSeed.size() &&
      std::memcmp(data, kInlineStyleComponentBudgetSeed.data(), size) == 0;

  if (forceXmlDataNodeBudget) {
    std::string svg = "<svg xmlns='http://www.w3.org/2000/svg'>";
    for (std::size_t i = 0; i < donner::xml::XMLParser::Options::kDefaultMaximumElements; ++i) {
      svg += "x<!---->";
    }
    svg += "</svg>";

    donner::ParseWarningSink disabled = donner::ParseWarningSink::Disabled();
    const auto result = donner::svg::parser::SVGParser::ParseSVG(svg, disabled);
    if (result.hasResult() ||
        result.error().reason.str().find("element count") == std::string_view::npos) {
      std::abort();
    }
    return 0;
  }
  FuzzedDataProvider provider(data, size);

  // Create an XMLDocument
  XMLDocument document;
  XMLNode root = document.root();

  // Optionally create an SVG element as the root
  const bool createSvgElement =
      forceNonFiniteFontSize || forceParsedPayloadBudget || forceStylesheetAggregateBudget ||
      forceDeferredPathPayloadBudget || forceChunkedTextProjection || forceChunkedStyleProjection ||
      forceChunkedMetadataProjection || forceInlineStyleComponentBudget || provider.ConsumeBool();
  if (createSvgElement) {
    // Create an SVG element
    XMLNode svgElement = XMLNode::CreateElementNode(document, "svg");
    svgElement.setAttribute("xmlns", "http://www.w3.org/2000/svg");
    AddEdgeCaseSizingAttributes(svgElement, provider);
    if (forceNonFiniteFontSize) {
      AddTextWithFontSize(document, svgElement, "1e999");
    } else {
      AddEdgeCaseText(document, svgElement, provider);
    }
    if (forceParsedPayloadBudget) {
      AddDistributedAttributePayload(document, svgElement);
    }
    if (forceStylesheetAggregateBudget) {
      AddDistributedStylesheets(document, svgElement);
    }
    if (forceDeferredPathPayloadBudget) {
      AddDistributedDeferredPaths(document, svgElement);
    }
    if (forceChunkedTextProjection) {
      AddChunkedProjection(document, svgElement, "text", "chunked-text", "x");
    }
    if (forceChunkedStyleProjection) {
      AddChunkedProjection(document, svgElement, "style", "chunked-style", "rect{fill:red}");
    }
    if (forceChunkedMetadataProjection) {
      AddChunkedProjection(document, svgElement, "metadata", "chunked-metadata", "x");
    }
    if (forceInlineStyleComponentBudget) {
      AddOversizedInlineStyle(document, svgElement);
    }
    root.appendChild(svgElement);

    // Set the SVG element as the new root for further processing
    root = svgElement;
  }

  // Build the XML tree
  if (!forceParsedPayloadBudget && !forceStylesheetAggregateBudget &&
      !forceDeferredPathPayloadBudget && !forceChunkedTextProjection &&
      !forceChunkedStyleProjection && !forceChunkedMetadataProjection &&
      !forceInlineStyleComponentBudget) {
    BuildXMLTree(document, root, provider);
  }

  // Pass the constructed document to the SVG parser
  donner::ParseWarningSink disabled = donner::ParseWarningSink::Disabled();
  donner::svg::parser::SVGParser::Options options;
  if (forceParsedPayloadBudget || forceStylesheetAggregateBudget) {
    options.maximumParsedPayloadSize = 64 * 1024;
  }
  if (forceChunkedTextProjection || forceChunkedStyleProjection || forceChunkedMetadataProjection) {
    options.maximumContentProjectionChunks = 32;
  }
  auto result =
      donner::svg::parser::SVGParser::ParseXMLDocument(std::move(document), disabled, options);
  if (result.hasResult()) {
    auto svgDocument = std::move(result).result();
    if (forceDeferredPathPayloadBudget) {
      svgDocument.unsafeRegistry()
          .ctx()
          .emplace<donner::svg::components::GeometryPreparationResourceBudget>(
              nullptr, donner::svg::components::GeometryPreparationResourceBudget::Limits{
                           .maximumBytes = 64 * 1024,
                       });
    }
    if (forceParsedPayloadBudget || forceStylesheetAggregateBudget) {
      const auto* budget = svgDocument.unsafeRegistry()
                               .ctx()
                               .find<donner::svg::components::ParsedPayloadResourceBudget>();
      if (budget == nullptr || !budget->securityStats().rejected ||
          budget->securityStats().retainedBytes > options.maximumParsedPayloadSize ||
          (forceParsedPayloadBudget && budget->securityStats().attributeBytes == 0) ||
          (forceStylesheetAggregateBudget && budget->securityStats().stylesheetBytes == 0)) {
        std::abort();
      }
    }
    if (forceChunkedTextProjection || forceChunkedStyleProjection ||
        forceChunkedMetadataProjection) {
      const auto* budget = svgDocument.unsafeRegistry()
                               .ctx()
                               .find<donner::svg::components::ParsedPayloadResourceBudget>();
      if (budget == nullptr || !budget->securityStats().rejected) {
        std::abort();
      }
    }
    if (forceChunkedTextProjection) {
      const auto element = svgDocument.querySelector("#chunked-text");
      const auto* component =
          element ? element->entityHandle().try_get<donner::svg::components::TextComponent>()
                  : nullptr;
      if (component == nullptr || !component->text.empty() || !component->textChunks.empty()) {
        std::abort();
      }
    }
    if (forceChunkedStyleProjection) {
      const auto element = svgDocument.querySelector("#chunked-style");
      const auto* component =
          element ? element->entityHandle().try_get<donner::svg::components::StylesheetComponent>()
                  : nullptr;
      // SVGStyleElement creates its StylesheetComponent lazily only after projection is accepted.
      // Rejection must therefore leave the element present without materializing CSS storage.
      if (!element || component != nullptr) {
        std::abort();
      }
    }
    if (forceChunkedMetadataProjection) {
      const auto element = svgDocument.querySelector("#chunked-metadata");
      if (!element ||
          element->entityHandle().try_get<donner::svg::components::DescriptiveTextComponent>() !=
              nullptr) {
        std::abort();
      }
    }
    if (forceInlineStyleComponentBudget) {
      const auto element = svgDocument.querySelector("#inline-style-component-budget");
      const auto* component =
          element ? element->entityHandle().try_get<donner::svg::components::StyleComponent>()
                  : nullptr;
      if (component == nullptr || !component->styleParseRejected ||
          component->properties.complexPropertyBytes() != 0) {
        std::abort();
      }
    }
    donner::svg::components::LayoutSystem layoutSystem;
    (void)layoutSystem.calculateCanvasScaledDocumentSize(
        svgDocument.unsafeRegistry(),
        donner::svg::components::LayoutSystem::InvalidSizeBehavior::ReturnDefault);
    donner::ParseWarningSink renderSink = donner::ParseWarningSink::Disabled();
    donner::svg::RendererUtils::prepareDocumentForRendering(svgDocument, /*verbose=*/false,
                                                            renderSink);
    if (forceDeferredPathPayloadBudget) {
      const auto* budget = svgDocument.unsafeRegistry()
                               .ctx()
                               .find<donner::svg::components::GeometryPreparationResourceBudget>();
      if (budget == nullptr || !budget->rejected() || budget->retainedBytes() == 0 ||
          budget->retainedBytes() > budget->limits().maximumBytes) {
        std::abort();
      }
    }
  }

  return 0;
}
