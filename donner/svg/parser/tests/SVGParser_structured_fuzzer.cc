#include <fuzzer/FuzzedDataProvider.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/parser/SVGParser.h"

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
    // List of known SVG element names
    // TODO: Update this to use AllSVGElements()
    constexpr std::string_view knownElementNames[] = {
        "circle",  "clipPath", "defs",           "ellipse", "feGaussianBlur", "filter", "g",
        "image",   "line",     "linearGradient", "marker",  "mask",           "path",   "pattern",
        "polygon", "polyline", "radialGradient", "rect",    "stop",           "style",  "svg",
        "unknown", "use"};

    const std::string_view elementName = provider.PickValueInArray(knownElementNames);
    return XMLQualifiedName(donner::RcString(elementName));
  }

  return CreateQualifiedName(provider);
}

// Function to create a random attribute name, possibly from known attributes
XMLQualifiedName CreateRandomAttributeName(FuzzedDataProvider& provider) {
  // Either pick from a known attribute name or generate a random one
  const bool useKnownAttributeName = provider.ConsumeBool();
  if (useKnownAttributeName) {
    // List of known attribute names
    // TODO: Share this with kValidPresentationAttributes in PropertyRegistry.cc
    constexpr std::string_view knownAttributeNames[] = {"cx",
                                                        "cy",
                                                        "height",
                                                        "width",
                                                        "x",
                                                        "y",
                                                        "r",
                                                        "rx",
                                                        "ry",
                                                        "d",
                                                        "fill",
                                                        "transform",
                                                        "alignment-baseline",
                                                        "baseline-shift",
                                                        "clip-path",
                                                        "clip-rule",
                                                        "color",
                                                        "color-interpolation",
                                                        "color-interpolation-filters",
                                                        "color-rendering",
                                                        "cursor",
                                                        "direction",
                                                        "display",
                                                        "dominant-baseline",
                                                        "fill-opacity",
                                                        "fill-rule",
                                                        "filter",
                                                        "flood-color",
                                                        "flood-opacity",
                                                        "font-family",
                                                        "font-size",
                                                        "font-size-adjust",
                                                        "font-stretch",
                                                        "font-style",
                                                        "font-variant",
                                                        "font-weight",
                                                        "glyph-orientation-horizontal",
                                                        "glyph-orientation-vertical",
                                                        "image-rendering",
                                                        "letter-spacing",
                                                        "lighting-color",
                                                        "marker-end",
                                                        "marker-mid",
                                                        "marker-start",
                                                        "mask",
                                                        "opacity",
                                                        "overflow",
                                                        "paint-order",
                                                        "pointer-events",
                                                        "shape-rendering",
                                                        "stop-color",
                                                        "stop-opacity",
                                                        "stroke",
                                                        "stroke-dasharray",
                                                        "stroke-dashoffset",
                                                        "stroke-linecap",
                                                        "stroke-linejoin",
                                                        "stroke-miterlimit",
                                                        "stroke-opacity",
                                                        "stroke-width",
                                                        "text-anchor",
                                                        "text-decoration",
                                                        "text-overflow",
                                                        "text-rendering",
                                                        "unicode-bidi",
                                                        "vector-effect",
                                                        "visibility",
                                                        "white-space",
                                                        "word-spacing",
                                                        "writing-mode"};

    const std::string_view attrName = provider.PickValueInArray(knownAttributeNames);
    return XMLQualifiedName(donner::RcString(attrName));
  } else {
    // Generate a random attribute name
    return CreateQualifiedName(provider);
  }
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
  FuzzedDataProvider provider(data, size);

  // Create an XMLDocument
  XMLDocument document;
  XMLNode root = document.root();

  // Optionally create an SVG element as the root
  const bool createSvgElement = provider.ConsumeBool();
  if (createSvgElement) {
    // Create an SVG element
    XMLNode svgElement = XMLNode::CreateElementNode(document, "svg");
    svgElement.setAttribute("xmlns", "http://www.w3.org/2000/svg");
    root.appendChild(svgElement);

    // Set the SVG element as the new root for further processing
    root = svgElement;
  }

  // Build the XML tree
  BuildXMLTree(document, root, provider);

  // Pass the constructed document to the SVG parser
  auto result = donner::svg::parser::SVGParser::ParseXMLDocument(std::move(document));
  (void)result;  // Suppress unused variable warning

  return 0;
}
