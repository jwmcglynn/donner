#include "donner/editor/ViewportSvgExport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/RcString.h"
#include "donner/base/StringUtils.h"
#include "donner/base/Vector2.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/OverlayRenderer.h"

namespace donner::editor {

namespace {

/// Preferred identifier for the clip path injected into exported documents.
/// If the source document already uses this id, a numeric suffix is appended
/// (see \ref UniqueClipPathId) so the injected clip path never collides.
constexpr std::string_view kClipPathId = "donner-viewport-clip";
/// Identifier for the editor overlay group.
constexpr std::string_view kOverlayGroupId = "donner-editor-overlay";

// Deterministic overlay styling. These constants are intentionally fixed and
// MUST stay independent of the ImGui editor theme: the exported overlay is a
// standalone artifact, so it cannot inherit live theme colors that drift
// between builds/themes. Changing them changes the exported chrome appearance.
//
/// Selection chrome stroke color (path outlines, AABBs, handles, marquee).
constexpr std::string_view kOverlayStroke = "#1ea7fd";
/// Resize-handle fill color.
constexpr std::string_view kOverlayHandleFill = "#ffffff";
constexpr std::string_view kOverlayOutlineClass = "donner-editor-overlay-outline";
constexpr std::string_view kOverlayLineClass = "donner-editor-overlay-line";
constexpr std::string_view kOverlayPointClass = "donner-editor-overlay-point";
constexpr std::string_view kOverlayHandleClass = "donner-editor-overlay-handle";

/// Format a double for SVG attribute output, trimming trailing zeros so
/// `100.0` becomes `100` and `12.50` becomes `12.5`. Determinism matters: the
/// exported `viewBox` is asserted against `screenToDocument(renderPaneRect)`.
std::string FormatNumber(double value) {
  if (!std::isfinite(value)) {
    return "0";
  }
  std::ostringstream stream;
  stream.precision(6);
  stream << std::fixed << value;
  std::string text = stream.str();
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') {
      text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  if (text == "-0") {
    return "0";
  }
  return text;
}

/// Append \p ch to \p out, escaped for XML. When \p inAttributeValue, tab, line feed and
/// carriage return become numeric character references: attribute-value normalization turns a
/// literal one into a space when the document is re-parsed, so an export carrying one literally
/// would not round-trip.
void AppendEscapedXmlChar(std::string& out, char ch, bool inAttributeValue) {
  switch (ch) {
    case '&': out += "&amp;"; return;
    case '<': out += "&lt;"; return;
    case '>': out += "&gt;"; return;
    case '"': out += "&quot;"; return;
    case '\'': out += "&apos;"; return;
    case '\t': out += inAttributeValue ? "&#9;" : "\t"; return;
    case '\n': out += inAttributeValue ? "&#10;" : "\n"; return;
    case '\r': out += inAttributeValue ? "&#13;" : "\r"; return;
    default: out += ch; return;
  }
}

/// Escape a string for inclusion in XML text content or a comment body.
std::string EscapeXml(std::string_view input) {
  std::string output;
  output.reserve(input.size());
  for (const char ch : input) {
    AppendEscapedXmlChar(output, ch, /*inAttributeValue=*/false);
  }
  return output;
}

/// Escape a string for inclusion in an XML attribute value.
std::string EscapeXmlAttributeValue(std::string_view input) {
  std::string output;
  output.reserve(input.size());
  for (const char ch : input) {
    AppendEscapedXmlChar(output, ch, /*inAttributeValue=*/true);
  }
  return output;
}

/// Sanitize a string for inclusion in an XML comment body. Applies the same
/// entity escaping as \ref EscapeXml, then additionally breaks up the sequence
/// "--" (forbidden inside an XML comment) and a trailing '-' (which would create
/// "--->"). Without the "--" handling an untrusted root `id` containing "--"
/// produces invalid XML that conformant consumers reject.
std::string EscapeXmlComment(std::string_view input) {
  const std::string escaped = EscapeXml(input);
  std::string output;
  output.reserve(escaped.size());
  char previous = '\0';
  for (const char ch : escaped) {
    if (ch == '-' && previous == '-') {
      output += ' ';
    }
    output += ch;
    previous = ch;
  }
  if (!output.empty() && output.back() == '-') {
    output += ' ';
  }
  return output;
}

/// A single `name="value"` attribute of the root element.
struct Attribute {
  std::string name;   ///< Qualified name in source spelling (`prefix:local` or `local`).
  std::string value;  ///< Parser-decoded value, escaped once when written out.
};

/// Root element markup derived from the parsed tree.
struct RootTag {
  /// Root tag in source spelling, `svg` or `prefix:svg`. The export re-emits the root under
  /// its own name so a prefixed root keeps resolving against the body's own declarations.
  std::string qualifiedName;
  /// Prefix (`` or `prefix:`) given to elements the export injects, so they land in the same
  /// namespace as the root rather than in no namespace under a prefixed root.
  std::string injectedPrefix;
  std::vector<Attribute> attributes;
  std::size_t bodyStart = 0;       ///< Byte offset just past the root open tag.
  std::size_t bodyEnd = 0;         ///< Byte offset of the root closing tag.
  bool locationsResolved = false;  ///< True once the offsets above resolved from the tree.
};

/// Find the document's root `<svg>` element: the first top-level element with local
/// name `svg`, matching `SVGParser`'s own root check (which additionally enforces the
/// namespace URI, so a wrong-namespace `<other:svg>` never reaches a parsed document).
std::optional<xml::XMLNode> FindRootSvgElement(const xml::XMLDocument& xmlDocument) {
  for (std::optional<xml::XMLNode> child = xmlDocument.root().firstXmlChild(); child.has_value();
       child = child->nextXmlSibling()) {
    if (child->type() != xml::XMLNode::Type::Element) {
      continue;
    }
    if (child->tagName().name == "svg") {
      return child;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

/// Resolve a source range to concrete offsets, or nullopt when unavailable or unordered.
std::optional<std::pair<std::size_t, std::size_t>> RangeOffsets(const SourceRange& range) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value() ||
      *range.start.offset > *range.end.offset) {
    return std::nullopt;
  }
  return std::pair{*range.start.offset, *range.end.offset};
}

/// Serialize a qualified name in source spelling (`prefix:local` or `local`).
std::string SerializeQualifiedName(const xml::XMLQualifiedNameRef& name) {
  std::string result;
  if (!name.namespacePrefix.empty()) {
    result.assign(name.namespacePrefix);
    result += ':';
  }
  result += std::string_view(name.name);
  return result;
}

/// Derive the root tag's name, attributes (source order, decoded values) and body bounds from
/// the parsed tree. Attribute order is recovered by sorting on each attribute's source offset,
/// since the tree stores attributes by name. Values are the parser-decoded values and are
/// escaped exactly once on output, so a source `&amp;` round-trips instead of becoming
/// `&amp;amp;`; the source offsets are used only for ordering.
RootTag DeriveRootTagFromTree(std::string_view source, const xml::XMLNode& root) {
  RootTag result;
  const std::optional<SourceRange> openTag = root.getOpeningTagLocation();
  const std::optional<std::pair<std::size_t, std::size_t>> openOffsets =
      openTag.has_value() ? RangeOffsets(*openTag) : std::nullopt;
  if (!openOffsets.has_value() || openOffsets->second > source.size()) {
    return result;
  }
  result.bodyStart = openOffsets->second;

  const std::optional<SourceRange> closeTag = root.getClosingTagLocation();
  if (!closeTag.has_value()) {
    // No closing tag is only sound for a childless (self-closing) root.
    if (root.firstXmlChild().has_value()) {
      return result;
    }
    result.bodyEnd = result.bodyStart;
  } else {
    const std::optional<std::pair<std::size_t, std::size_t>> closeOffsets = RangeOffsets(*closeTag);
    if (!closeOffsets.has_value() || closeOffsets->first < result.bodyStart ||
        closeOffsets->second > source.size()) {
      return result;
    }
    result.bodyEnd = closeOffsets->first;
  }

  struct OrderedAttribute {
    std::size_t offset = 0;
    bool anchored = false;
    Attribute attribute;
  };
  std::vector<OrderedAttribute> ordered;
  for (const xml::XMLQualifiedNameRef& name : root.attributes()) {
    OrderedAttribute entry;
    entry.attribute.name = SerializeQualifiedName(name);
    if (const std::optional<RcString> decoded = root.getAttribute(name); decoded.has_value()) {
      entry.attribute.value.assign(std::string_view(*decoded));
    }
    if (const std::optional<xml::XMLAttributeSourceLocation> location =
            root.getAttributeSourceLocation(name);
        location.has_value()) {
      if (const std::optional<std::pair<std::size_t, std::size_t>> fullOffsets =
              RangeOffsets(location->fullRange);
          fullOffsets.has_value()) {
        entry.offset = fullOffsets->first;
        entry.anchored = true;
      }
    }
    ordered.push_back(std::move(entry));
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const OrderedAttribute& lhs, const OrderedAttribute& rhs) {
                     if (lhs.anchored != rhs.anchored) {
                       return lhs.anchored;
                     }
                     return lhs.offset < rhs.offset;
                   });
  for (auto& entry : ordered) {
    result.attributes.push_back(std::move(entry.attribute));
  }
  // A prefixed root is only re-emitted prefixed when the prefix is actually bound to the SVG
  // namespace. Inline-SVG parsing repairs an unbound root by injecting a default `xmlns`, which
  // leaves the prefix undeclared: re-emitting it, and prefixing the injected elements with it,
  // would produce markup no consumer can resolve.
  const xml::XMLQualifiedNameRef rootName = root.tagName();
  const bool prefixBoundToSvg =
      !rootName.namespacePrefix.empty() &&
      root.getNamespaceUri(rootName.namespacePrefix) == "http://www.w3.org/2000/svg";
  if (prefixBoundToSvg) {
    result.qualifiedName = SerializeQualifiedName(rootName);
    result.injectedPrefix = std::string(std::string_view(rootName.namespacePrefix)) + ":";
  } else {
    result.qualifiedName = std::string(std::string_view(rootName.name));
  }
  result.locationsResolved = true;
  return result;
}

/// Returns true when an attribute local name ends with "href" (case-sensitive), preserving
/// the historical match for `href`, `xlink:href`, `data-href`, and friends.
bool IsHrefAttributeName(std::string_view localName) {
  return StringUtils::EndsWith(localName, std::string_view("href"));
}

/// Check every element attribute in the parsed tree for external `href` references over
/// `http://`, `https://`, or `file://`. Only real parsed attributes are inspected, and
/// values are entity-decoded by the parser, so encoded schemes cannot evade the match.
/// Returns the offending decoded value, or empty.
std::string FindExternalReference(const xml::XMLNode& root) {
  static constexpr std::array<std::string_view, 3> kExternalSchemes = {
      "http://",
      "https://",
      "file://",
  };
  // Iterative pre-order walk (explicit stack, children pushed in reverse): no recursion
  // depth risk on deep documents, deterministic document order for the first refusal.
  std::vector<xml::XMLNode> stack{root};
  while (!stack.empty()) {
    const xml::XMLNode node = stack.back();
    stack.pop_back();
    if (node.type() == xml::XMLNode::Type::Element) {
      for (const xml::XMLQualifiedNameRef& name : node.attributes()) {
        if (!IsHrefAttributeName(name.name)) {
          continue;
        }
        const std::optional<RcString> value = node.getAttribute(name);
        if (!value.has_value()) {
          continue;
        }
        const std::string_view view(*value);
        const std::string_view trimmed = StringUtils::TrimWhitespace(view);
        for (const std::string_view scheme : kExternalSchemes) {
          if (StringUtils::StartsWith<StringComparison::IgnoreCase>(trimmed, scheme)) {
            return std::string(view);
          }
        }
      }
    }
    std::vector<xml::XMLNode> children;
    for (std::optional<xml::XMLNode> child = node.firstXmlChild(); child.has_value();
         child = child->nextXmlSibling()) {
      children.push_back(*child);
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(*it);
    }
  }
  return std::string();
}

std::string OverlayPaintDeclarations(std::string_view fill, std::string_view stroke,
                                     std::string_view strokeWidth) {
  return "fill: " + std::string(fill) + " !important; stroke: " + std::string(stroke) +
         " !important; stroke-width: " + std::string(strokeWidth) +
         " !important; fill-opacity: 1 !important; stroke-opacity: 1 !important;";
}

std::string OverlayPaintStyle(std::string_view fill, std::string_view stroke,
                              std::string_view strokeWidth) {
  return " style=\"" + OverlayPaintDeclarations(fill, stroke, strokeWidth) + "\"";
}

std::string OverlayStylesheet(std::string_view prefix, std::string_view overlayGroupId) {
  std::string out = "<" + std::string(prefix) + "style>";
  const auto appendRule = [&out, overlayGroupId](std::string_view className, std::string_view fill,
                                                 std::string_view stroke,
                                                 std::string_view strokeWidth) {
    out += "#" + std::string(overlayGroupId) + " ." + std::string(className) + "{" +
           OverlayPaintDeclarations(fill, stroke, strokeWidth) + "}";
  };
  appendRule(kOverlayOutlineClass, "none", kOverlayStroke, "1.5");
  appendRule(kOverlayLineClass, "none", kOverlayStroke, "1");
  appendRule(kOverlayPointClass, kOverlayStroke, "none", "0");
  appendRule(kOverlayHandleClass, kOverlayHandleFill, kOverlayStroke, "1");
  out += "</" + std::string(prefix) + "style>";
  return out;
}

/// Append a `<rect>` element for a document-space box with explicit fill/stroke.
void AppendRect(std::string* out, std::string_view prefix, const Box2d& boxDoc,
                std::string_view fill, std::string_view stroke, std::string_view strokeWidth,
                std::string_view className) {
  *out += "<" + std::string(prefix) + "rect x=\"" + FormatNumber(boxDoc.topLeft.x) + "\" y=\"" +
          FormatNumber(boxDoc.topLeft.y) + "\" width=\"" + FormatNumber(boxDoc.width()) +
          "\" height=\"" + FormatNumber(boxDoc.height()) + "\" fill=\"" + std::string(fill) +
          "\" stroke=\"" + std::string(stroke) + "\" stroke-width=\"" + std::string(strokeWidth) +
          "\" class=\"" + std::string(className) + "\"" +
          OverlayPaintStyle(fill, stroke, strokeWidth) + "/>";
}

/// Append a closed `<path>` element tracing the four corners of an oriented box.
void AppendOrientedBox(std::string* out, std::string_view prefix,
                       const std::array<Vector2d, 4>& cornersDoc) {
  std::string d = "M " + FormatNumber(cornersDoc[0].x) + " " + FormatNumber(cornersDoc[0].y);
  for (std::size_t i = 1; i < cornersDoc.size(); ++i) {
    d += " L " + FormatNumber(cornersDoc[i].x) + " " + FormatNumber(cornersDoc[i].y);
  }
  d += " Z";
  *out += "<" + std::string(prefix) + "path d=\"" + EscapeXmlAttributeValue(d) +
          "\" fill=\"none\" stroke=\"" + std::string(kOverlayStroke) +
          "\" stroke-width=\"1\" class=\"" + std::string(kOverlayLineClass) + "\"" +
          OverlayPaintStyle("none", kOverlayStroke, "1") + "/>";
}

/// Append an open line path for a path control-handle guide.
void AppendControlLine(std::string* out, std::string_view prefix,
                       const SelectionChromeSnapshot::PathControlLine& lineDoc) {
  const std::string d =
      "M " + FormatNumber(lineDoc.anchorDoc.x) + " " + FormatNumber(lineDoc.anchorDoc.y) + " L " +
      FormatNumber(lineDoc.controlDoc.x) + " " + FormatNumber(lineDoc.controlDoc.y);
  *out += "<" + std::string(prefix) + "path d=\"" + EscapeXmlAttributeValue(d) +
          "\" fill=\"none\" stroke=\"" + std::string(kOverlayStroke) +
          "\" stroke-width=\"1\" class=\"" + std::string(kOverlayLineClass) + "\"" +
          OverlayPaintStyle("none", kOverlayStroke, "1") +
          " vector-effect=\"non-scaling-stroke\"/>";
}

/// Returns true if \p document declares an element with the given \p id.
/// Querying the parsed DOM handles legal XML whitespace and character
/// references that a raw-source substring scan would miss.
bool DocumentDeclaresId(const svg::SVGDocument& document, std::string_view id) {
  svg::SVGSVGElement root = document.svgElement();
  if (const std::optional<RcString> rootId = root.getAttribute("id"); rootId == id) {
    return true;
  }

  return root.querySelector("#" + std::string(id)).has_value();
}

/// Pick an injected id that does not collide with any id already declared in
/// the source document. Prefers \p baseId and appends an increasing numeric
/// suffix (`-2`, `-3`, ...) until the id is unused.
std::string UniqueInjectedId(const svg::SVGDocument& document, std::string_view baseId) {
  std::string id(baseId);
  for (int suffix = 2; DocumentDeclaresId(document, id); ++suffix) {
    id = std::string(baseId) + "-" + std::to_string(suffix);
  }
  return id;
}

}  // namespace

std::string SerializeOverlaySnapshotToSvg(const SelectionChromeSnapshot& snapshot,
                                          std::string_view elementPrefix) {
  std::string out;

  // Selected path outlines. `vector-effect="non-scaling-stroke"` keeps the 1.5px
  // chrome stroke constant regardless of the export viewBox scale.
  for (const SelectionChromeSnapshot::PathItem& item : snapshot.paths) {
    const RcString pathData = item.pathDoc.toSVGPathData();
    if (pathData.empty()) {
      continue;
    }
    out += "<" + std::string(elementPrefix) + "path d=\"" +
           EscapeXmlAttributeValue(pathData.str()) + "\" fill=\"none\" stroke=\"" +
           std::string(kOverlayStroke) + "\" stroke-width=\"1.5\" class=\"" +
           std::string(kOverlayOutlineClass) + "\"" +
           OverlayPaintStyle("none", kOverlayStroke, "1.5") +
           " vector-effect=\"non-scaling-stroke\"/>";
  }

  // Selected path Bezier control lines and points.
  for (const SelectionChromeSnapshot::PathControlLine& controlLineDoc :
       snapshot.pathControlLinesDoc) {
    AppendControlLine(&out, elementPrefix, controlLineDoc);
  }
  for (const Vector2d& controlPointDoc : snapshot.pathControlPointsDoc) {
    AppendRect(&out, elementPrefix,
               OverlayRenderer::ChromeSquareForPoint(
                   snapshot, OverlayRenderer::ChromeSquare::PathControlPoint, controlPointDoc),
               kOverlayStroke, "none", "0", kOverlayPointClass);
  }
  for (const Vector2d& anchorDoc : snapshot.pathAnchorPointsDoc) {
    AppendRect(&out, elementPrefix,
               OverlayRenderer::ChromeSquareForPoint(
                   snapshot, OverlayRenderer::ChromeSquare::PathAnchor, anchorDoc),
               kOverlayStroke, "none", "0", kOverlayPointClass);
  }

  // Selection AABBs.
  for (const Box2d& aabbDoc : snapshot.aabbsDoc) {
    AppendRect(&out, elementPrefix, aabbDoc, "none", kOverlayStroke, "1", kOverlayLineClass);
  }

  // Oriented rotation box (drawn instead of axis-aligned AABBs during rotation).
  if (snapshot.orientedBoundsDoc.has_value()) {
    AppendOrientedBox(&out, elementPrefix, snapshot.orientedBoundsDoc->cornersDoc);
  }

  // Resize handles: small filled squares.
  for (const Vector2d& handleAnchorDoc : snapshot.handleAnchorsDoc) {
    AppendRect(&out, elementPrefix,
               OverlayRenderer::ChromeSquareForPoint(
                   snapshot, OverlayRenderer::ChromeSquare::TransformHandle, handleAnchorDoc),
               kOverlayHandleFill, kOverlayStroke, "1", kOverlayHandleClass);
  }

  // Marquee rect.
  if (snapshot.marqueeDoc.has_value()) {
    AppendRect(&out, elementPrefix, *snapshot.marqueeDoc, "none", kOverlayStroke, "1",
               kOverlayLineClass);
  }

  return out;
}

Result<std::string, std::string> ExportViewportAsSvg(
    const svg::SVGDocument& doc, const ViewportState& viewport, const Recti& renderPaneRect,
    const ViewportExportOptions& options, const SelectionChromeSnapshot* overlaySnapshot) {
  using ResultType = Result<std::string, std::string>;

  if (!doc.hasSourceStore()) {
    return ResultType::Err("Viewport export requires a document with an XML source store.");
  }

  // Everything below derives from the parsed tree, so refuse while the tree is stale
  // relative to the source rather than slicing current bytes with old ranges.
  const xml::XMLDocument xmlDocument = doc.xmlDocument();
  if (const std::optional<ParseDiagnostic> stale = xmlDocument.sourceDiagnostic()) {
    return ResultType::Err("Viewport export requires a document whose source parses cleanly: " +
                           stale->reason.str() + ".");
  }

  // The parser expands entity references and does not keep the DOCTYPE declarations, so a body
  // carrying `&name;` would export with nothing to resolve it against. Refuse from what the
  // parser resolved rather than inspecting the prolog bytes.
  if (xmlDocument.declaresDoctypeInternalSubset()) {
    return ResultType::Err(
        "Viewport export cannot reproduce a DOCTYPE internal subset: the parser expands and drops "
        "the declarations, so an exported body could reference an undeclared entity. Inline the "
        "entity values and try again.");
  }

  const std::string_view source = doc.source();

  // Refuse `href`-suffixed attributes (`href`, `xlink:href`, `data-href`) whose decoded value
  // targets http://, https://, or file://. This is the whole check: CSS `url()` references in
  // `style` attributes or `<style>` elements are not inspected.
  const std::string externalReference = FindExternalReference(xmlDocument.root());
  if (!externalReference.empty()) {
    return ResultType::Err(
        "Viewport export cannot embed the external href reference: " + externalReference +
        ". Inline or remove http://, https://, and file:// href/xlink:href references and try "
        "again.");
  }

  const std::optional<xml::XMLNode> rootNode = FindRootSvgElement(xmlDocument);
  if (!rootNode.has_value()) {
    return ResultType::Err("Viewport export could not find the root <svg> element in the source.");
  }
  const RootTag rootTag = DeriveRootTagFromTree(source, *rootNode);
  if (!rootTag.locationsResolved) {
    return ResultType::Err(
        "Viewport export could not resolve the source locations of the root <svg> element.");
  }
  const std::string& injectedPrefix = rootTag.injectedPrefix;

  // Id for the injected clip path, uniquified against ids already declared in
  // the source so a document that defines "donner-viewport-clip" itself does
  // not collide with the injected definition.
  const std::string clipPathId = UniqueInjectedId(doc, kClipPathId);
  const std::string overlayGroupId = UniqueInjectedId(doc, kOverlayGroupId);

  // Compute the document-space viewport rect from the screen-space pane rect.
  // `ViewportState` is the single source of truth for crop and scale.
  const Box2d paneScreenBox(Vector2d(static_cast<double>(renderPaneRect.topLeft.x),
                                     static_cast<double>(renderPaneRect.topLeft.y)),
                            Vector2d(static_cast<double>(renderPaneRect.bottomRight.x),
                                     static_cast<double>(renderPaneRect.bottomRight.y)));
  const Box2d documentViewportBox = viewport.screenToDocument(paneScreenBox);

  const double viewBoxMinX = documentViewportBox.topLeft.x;
  const double viewBoxMinY = documentViewportBox.topLeft.y;
  const double viewBoxWidth = documentViewportBox.width();
  const double viewBoxHeight = documentViewportBox.height();

  // Output dimensions match the render pane size in CSS pixels.
  const int outputWidth = renderPaneRect.bottomRight.x - renderPaneRect.topLeft.x;
  const int outputHeight = renderPaneRect.bottomRight.y - renderPaneRect.topLeft.y;

  const std::string viewBoxValue = FormatNumber(viewBoxMinX) + " " + FormatNumber(viewBoxMinY) +
                                   " " + FormatNumber(viewBoxWidth) + " " +
                                   FormatNumber(viewBoxHeight);

  // Source document name for provenance metadata (no absolute local paths).
  std::string sourceName = "untitled";
  {
    // The provenance metadata only needs a source document name. We do not
    // have a filename here (export takes the document, not a path), so use the
    // root element id when present, else "untitled". This keeps metadata free
    // of absolute local paths.
    for (const Attribute& attribute : rootTag.attributes) {
      if (attribute.name == "id" && !attribute.value.empty()) {
        sourceName = attribute.value;
        break;
      }
    }
  }

  std::string output;
  output.reserve(source.size() + 1024);

  output += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  output += "<!-- Generated by Donner SVG Editor & Engine; source: ";
  output += EscapeXmlComment(sourceName);
  output += "; viewBox: ";
  output += EscapeXmlComment(viewBoxValue);
  output += " -->\n";

  // Root open tag: carry source root attributes, replacing viewBox/width/height.
  output += "<" + rootTag.qualifiedName;
  bool wroteViewBox = false;
  bool wroteWidth = false;
  bool wroteHeight = false;
  for (const Attribute& attribute : rootTag.attributes) {
    if (attribute.name == "viewBox") {
      output += " viewBox=\"" + EscapeXmlAttributeValue(viewBoxValue) + "\"";
      wroteViewBox = true;
    } else if (attribute.name == "width") {
      output += " width=\"" + std::to_string(outputWidth) + "\"";
      wroteWidth = true;
    } else if (attribute.name == "height") {
      output += " height=\"" + std::to_string(outputHeight) + "\"";
      wroteHeight = true;
    } else {
      output += " " + attribute.name + "=\"" + EscapeXmlAttributeValue(attribute.value) + "\"";
    }
  }
  if (!wroteWidth) {
    output += " width=\"" + std::to_string(outputWidth) + "\"";
  }
  if (!wroteHeight) {
    output += " height=\"" + std::to_string(outputHeight) + "\"";
  }
  if (!wroteViewBox) {
    output += " viewBox=\"" + EscapeXmlAttributeValue(viewBoxValue) + "\"";
  }
  output += ">\n";

  // Defs: clip path covering the document-space viewport rect.
  output += "  <" + injectedPrefix + "defs><" + injectedPrefix + "clipPath id=\"";
  output += clipPathId;
  output += "\"><" + injectedPrefix + "rect x=\"" + FormatNumber(viewBoxMinX) + "\" y=\"" +
            FormatNumber(viewBoxMinY) + "\" width=\"" + FormatNumber(viewBoxWidth) +
            "\" height=\"" + FormatNumber(viewBoxHeight) + "\"/></" + injectedPrefix +
            "clipPath></" + injectedPrefix + "defs>\n";

  // Optional covering background rect (non-transparent export).
  if (!options.transparentBackground) {
    output += "  <" + injectedPrefix + "rect x=\"" + FormatNumber(viewBoxMinX) + "\" y=\"" +
              FormatNumber(viewBoxMinY) + "\" width=\"" + FormatNumber(viewBoxWidth) +
              "\" height=\"" + FormatNumber(viewBoxHeight) + "\" fill=\"#ffffff\"/>\n";
  }

  // Source stylesheets remain active in the exported SVG. Define the editor
  // chrome rules before the imported content so Donner's stylesheet traversal
  // applies them last, while the scoped selector and !important declarations
  // also give them the intended precedence in standards-compliant browsers.
  if (options.includeSelectionOverlay && overlaySnapshot != nullptr) {
    output += "  ";
    output += OverlayStylesheet(injectedPrefix, overlayGroupId);
    output += "\n";
  }

  // Document content: source children verbatim, wrapped in a clipped group.
  output += "  <" + injectedPrefix + "g clip-path=\"url(#";
  output += clipPathId;
  output += ")\">";
  if (rootTag.bodyEnd > rootTag.bodyStart) {
    output += source.substr(rootTag.bodyStart, rootTag.bodyEnd - rootTag.bodyStart);
  }
  output += "</" + injectedPrefix + "g>\n";

  // Optional editor overlay group. Populated from the captured selection-chrome
  // snapshot when one is supplied; otherwise emitted empty (back-compat).
  // Clipped to the same document-space viewport rect as the content so overlay
  // chrome never spills outside the exported crop.
  if (options.includeSelectionOverlay) {
    output += "  <" + injectedPrefix + "g id=\"";
    output += overlayGroupId;
    output +=
        "\" data-donner-export-role=\"editor-overlay\" pointer-events=\"none\" clip-path=\"url(#";
    output += clipPathId;
    output += ")\">";
    if (overlaySnapshot != nullptr) {
      output += SerializeOverlaySnapshotToSvg(*overlaySnapshot, injectedPrefix);
    }
    output += "</" + injectedPrefix + "g>\n";
  }

  output += "</" + rootTag.qualifiedName + ">\n";

  return ResultType::Ok(std::move(output));
}

}  // namespace donner::editor
