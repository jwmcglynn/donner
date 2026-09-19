#include "donner/editor/ViewportSvgExport.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/RcString.h"
#include "donner/base/Vector2.h"
#include "donner/base/xml/XMLTokenizer.h"
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

/// Escape a string for inclusion in an XML attribute value or text content.
std::string EscapeXml(std::string_view input) {
  std::string output;
  output.reserve(input.size());
  for (const char ch : input) {
    switch (ch) {
      case '&': output += "&amp;"; break;
      case '<': output += "&lt;"; break;
      case '>': output += "&gt;"; break;
      case '"': output += "&quot;"; break;
      case '\'': output += "&apos;"; break;
      default: output += ch; break;
    }
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

/// Case-insensitive ASCII comparison of \p haystack starting at \p pos against
/// \p needle.
bool MatchesAtCaseInsensitive(std::string_view haystack, std::size_t pos, std::string_view needle) {
  if (pos + needle.size() > haystack.size()) {
    return false;
  }
  for (std::size_t i = 0; i < needle.size(); ++i) {
    char a = haystack[pos + i];
    char b = needle[i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
      b = static_cast<char>(b - 'A' + 'a');
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

/// A single `name="value"` attribute parsed from an element open tag.
struct Attribute {
  std::string name;
  std::string value;  ///< Raw value bytes as they appeared in the source.
};

/// Parsed root `<svg ...>` open tag.
struct RootTag {
  std::vector<Attribute> attributes;
  std::size_t bodyStart = 0;  ///< Byte offset just past the root open tag.
  std::size_t bodyEnd = 0;    ///< Byte offset of the root `</svg>` close tag.
  bool found = false;
};

/// Returns true if \p text is the element name "svg" in ASCII
/// case-insensitive comparison. The token stream already isolated the name, so
/// no terminator check is needed.
bool IsSvgTagName(std::string_view text) {
  return text.size() == 3 && MatchesAtCaseInsensitive(text, 0, "svg");
}

/// Byte offsets of a token's source range. The token stream is gap-free, so
/// every token resolves to concrete offsets.
std::size_t TokenStart(const xml::XMLToken& token) {
  return token.range.start.offset.value_or(0);
}
std::size_t TokenEnd(const xml::XMLToken& token) {
  return token.range.end.offset.value_or(TokenStart(token));
}

/// Tokenize \p source with the shared XML tokenizer. All markup-boundary
/// decisions below come from this stream: comments, CDATA sections, doctypes,
/// processing instructions, and quoted attribute values are opaque single
/// tokens, so markup-like text inside them can never be mistaken for elements.
std::vector<xml::XMLToken> TokenizeSource(std::string_view source) {
  std::vector<xml::XMLToken> tokens;
  xml::Tokenize(source, [&](xml::XMLToken token) { tokens.push_back(token); });
  return tokens;
}

/// Strip one surrounding quote pair from a quoted attribute-value token. The
/// tokenizer only emits `AttributeValue` for values with both delimiters
/// present, so the first and last bytes are always the quote characters.
std::string_view UnquoteValue(std::string_view quoted) {
  if (quoted.size() >= 2) {
    return quoted.substr(1, quoted.size() - 2);
  }
  return std::string_view();
}

/// Result of locating the end of an element open tag in the token stream.
struct TagEnd {
  std::size_t index = 0;  ///< Index of the TagClose or TagSelfClose token.
  bool selfClosing = false;
  bool complete = false;  ///< False when the tag never terminates.
};

/// Find the TagClose or TagSelfClose token terminating the open tag whose
/// TagName is at \p nameIndex. Returns an incomplete result when error
/// recovery intervenes: an unterminated tag is not an element boundary.
TagEnd FindOpenTagEnd(const std::vector<xml::XMLToken>& tokens, std::size_t nameIndex) {
  TagEnd result;
  for (std::size_t j = nameIndex + 1; j < tokens.size(); ++j) {
    const xml::XMLTokenType type = tokens[j].type;
    if (type == xml::XMLTokenType::TagClose) {
      result.index = j;
      result.complete = true;
      return result;
    }
    if (type == xml::XMLTokenType::TagSelfClose) {
      result.index = j;
      result.selfClosing = true;
      result.complete = true;
      return result;
    }
    if (type == xml::XMLTokenType::TagOpen || type == xml::XMLTokenType::ErrorRecovery) {
      return result;
    }
  }
  return result;
}

/// Parse the root `<svg>` open tag attributes and locate the body bounds from
/// the shared token stream. The first top-level `<svg>` element is the root;
/// nested `<svg>` elements are tracked so an inner close cannot be mistaken for
/// the root's. A missing root close tag tolerates slicing to end-of-source
/// (live editing may export mid-edit); anything that prevents locating a
/// complete root open tag fails closed with `found == false`.
RootTag ParseRootTag(std::string_view source, const std::vector<xml::XMLToken>& tokens) {
  RootTag result;
  std::size_t depth = 0;
  for (std::size_t i = 0; i < tokens.size();) {
    if (tokens[i].type != xml::XMLTokenType::TagOpen || i + 1 >= tokens.size() ||
        tokens[i + 1].type != xml::XMLTokenType::TagName) {
      ++i;
      continue;
    }
    const bool isClosing = tokens[i].text(source).starts_with("</");
    const std::string_view name = tokens[i + 1].text(source);
    if (isClosing) {
      if (depth > 0) {
        --depth;
      }
      i += 2;  // The close tag's own TagClose needs no handling.
      continue;
    }
    const TagEnd tagEnd = FindOpenTagEnd(tokens, i + 1);
    if (!tagEnd.complete) {
      ++i;  // An unterminated tag is not an element boundary.
      continue;
    }
    if (depth == 0 && IsSvgTagName(name)) {
      // Collect the root attributes in source order. Values are the raw source
      // bytes (entities intact); the caller re-escapes them on output.
      std::optional<std::size_t> awaitingValue;
      for (std::size_t k = i + 2; k < tagEnd.index; ++k) {
        if (tokens[k].type == xml::XMLTokenType::AttributeName) {
          Attribute attribute;
          attribute.name.assign(tokens[k].text(source));
          result.attributes.push_back(std::move(attribute));
          awaitingValue = result.attributes.size() - 1;
        } else if (tokens[k].type == xml::XMLTokenType::AttributeValue &&
                   awaitingValue.has_value()) {
          result.attributes[*awaitingValue].value.assign(UnquoteValue(tokens[k].text(source)));
          awaitingValue.reset();
        }
      }
      result.bodyStart = TokenEnd(tokens[tagEnd.index]);
      if (tagEnd.selfClosing) {
        result.bodyEnd = result.bodyStart;  // No children.
        result.found = true;
        return result;
      }
      // Find the matching close tag, tracking nested `<svg>` elements. A
      // `</svg` prefix counts as a close without requiring its `>`, matching
      // the historical scan; anything else (including error recovery) is
      // skipped.
      std::size_t svgDepth = 1;
      for (std::size_t k = tagEnd.index + 1; k < tokens.size();) {
        if (tokens[k].type != xml::XMLTokenType::TagOpen || k + 1 >= tokens.size() ||
            tokens[k + 1].type != xml::XMLTokenType::TagName) {
          ++k;
          continue;
        }
        if (!IsSvgTagName(tokens[k + 1].text(source))) {
          ++k;
          continue;
        }
        if (tokens[k].text(source).starts_with("</")) {
          --svgDepth;
          if (svgDepth == 0) {
            result.bodyEnd = TokenStart(tokens[k]);
            result.found = true;
            return result;
          }
          ++k;
          continue;
        }
        const TagEnd nestedEnd = FindOpenTagEnd(tokens, k + 1);
        if (nestedEnd.complete && !nestedEnd.selfClosing) {
          ++svgDepth;
          k = nestedEnd.index + 1;
        } else {
          ++k;
        }
      }
      result.bodyEnd = source.size();  // No matching close tag; use the remainder.
      result.found = true;
      return result;
    }
    if (!tagEnd.selfClosing) {
      ++depth;
    }
    i = tagEnd.index + 1;
  }
  return result;
}

/// Scan attribute tokens for external `href` / `xlink:href` references over
/// `http://`, `https://`, or `file://`. Only real parsed attributes are
/// inspected: `href`-like text in comments, CDATA sections, processing
/// instructions, or element text never forms AttributeName tokens. An
/// attribute matches when its name ends with "href" (case-sensitive),
/// preserving the historical substring match for real attributes. Returns the
/// offending raw value, or empty.
std::string FindExternalReference(std::string_view source,
                                  const std::vector<xml::XMLToken>& tokens) {
  static constexpr std::array<std::string_view, 3> kExternalSchemes = {
      "http://",
      "https://",
      "file://",
  };
  bool awaitingHrefValue = false;
  for (const xml::XMLToken& token : tokens) {
    switch (token.type) {
      case xml::XMLTokenType::AttributeName: {
        const std::string_view name = token.text(source);
        awaitingHrefValue = name.size() >= 4 && name.substr(name.size() - 4) == "href";
        break;
      }
      case xml::XMLTokenType::AttributeValue:
        if (awaitingHrefValue) {
          awaitingHrefValue = false;
          const std::string_view value = UnquoteValue(token.text(source));
          // Skip leading whitespace inside the value when scheme-matching.
          std::size_t valueOffset = 0;
          while (valueOffset < value.size() &&
                 (value[valueOffset] == ' ' || value[valueOffset] == '\t')) {
            ++valueOffset;
          }
          for (const std::string_view scheme : kExternalSchemes) {
            if (MatchesAtCaseInsensitive(value, valueOffset, scheme)) {
              return std::string(value);
            }
          }
        }
        break;
      case xml::XMLTokenType::Whitespace:
        break;  // Only whitespace intervenes between a name and its value.
      default: awaitingHrefValue = false; break;
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

std::string OverlayStylesheet(std::string_view overlayGroupId) {
  std::string out = "<style>";
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
  out += "</style>";
  return out;
}

/// Append a `<rect>` element for a document-space box with explicit fill/stroke.
void AppendRect(std::string* out, const Box2d& boxDoc, std::string_view fill,
                std::string_view stroke, std::string_view strokeWidth, std::string_view className) {
  *out += "<rect x=\"" + FormatNumber(boxDoc.topLeft.x) + "\" y=\"" +
          FormatNumber(boxDoc.topLeft.y) + "\" width=\"" + FormatNumber(boxDoc.width()) +
          "\" height=\"" + FormatNumber(boxDoc.height()) + "\" fill=\"" + std::string(fill) +
          "\" stroke=\"" + std::string(stroke) + "\" stroke-width=\"" + std::string(strokeWidth) +
          "\" class=\"" + std::string(className) + "\"" +
          OverlayPaintStyle(fill, stroke, strokeWidth) + "/>";
}

/// Append a closed `<path>` element tracing the four corners of an oriented box.
void AppendOrientedBox(std::string* out, const std::array<Vector2d, 4>& cornersDoc) {
  std::string d = "M " + FormatNumber(cornersDoc[0].x) + " " + FormatNumber(cornersDoc[0].y);
  for (std::size_t i = 1; i < cornersDoc.size(); ++i) {
    d += " L " + FormatNumber(cornersDoc[i].x) + " " + FormatNumber(cornersDoc[i].y);
  }
  d += " Z";
  *out += "<path d=\"" + EscapeXml(d) + "\" fill=\"none\" stroke=\"" + std::string(kOverlayStroke) +
          "\" stroke-width=\"1\" class=\"" + std::string(kOverlayLineClass) + "\"" +
          OverlayPaintStyle("none", kOverlayStroke, "1") + "/>";
}

/// Append an open line path for a path control-handle guide.
void AppendControlLine(std::string* out, const SelectionChromeSnapshot::PathControlLine& lineDoc) {
  const std::string d =
      "M " + FormatNumber(lineDoc.anchorDoc.x) + " " + FormatNumber(lineDoc.anchorDoc.y) + " L " +
      FormatNumber(lineDoc.controlDoc.x) + " " + FormatNumber(lineDoc.controlDoc.y);
  *out += "<path d=\"" + EscapeXml(d) + "\" fill=\"none\" stroke=\"" + std::string(kOverlayStroke) +
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

std::string SerializeOverlaySnapshotToSvg(const SelectionChromeSnapshot& snapshot) {
  std::string out;

  // Selected path outlines. `vector-effect="non-scaling-stroke"` keeps the 1.5px
  // chrome stroke constant regardless of the export viewBox scale.
  for (const SelectionChromeSnapshot::PathItem& item : snapshot.paths) {
    const RcString pathData = item.pathDoc.toSVGPathData();
    if (pathData.empty()) {
      continue;
    }
    out += "<path d=\"" + EscapeXml(pathData.str()) + "\" fill=\"none\" stroke=\"" +
           std::string(kOverlayStroke) + "\" stroke-width=\"1.5\" class=\"" +
           std::string(kOverlayOutlineClass) + "\"" +
           OverlayPaintStyle("none", kOverlayStroke, "1.5") +
           " vector-effect=\"non-scaling-stroke\"/>";
  }

  // Selected path Bezier control lines and points.
  for (const SelectionChromeSnapshot::PathControlLine& controlLineDoc :
       snapshot.pathControlLinesDoc) {
    AppendControlLine(&out, controlLineDoc);
  }
  for (const Vector2d& controlPointDoc : snapshot.pathControlPointsDoc) {
    AppendRect(&out,
               OverlayRenderer::ChromeSquareForPoint(
                   snapshot, OverlayRenderer::ChromeSquare::PathControlPoint, controlPointDoc),
               kOverlayStroke, "none", "0", kOverlayPointClass);
  }
  for (const Vector2d& anchorDoc : snapshot.pathAnchorPointsDoc) {
    AppendRect(&out,
               OverlayRenderer::ChromeSquareForPoint(
                   snapshot, OverlayRenderer::ChromeSquare::PathAnchor, anchorDoc),
               kOverlayStroke, "none", "0", kOverlayPointClass);
  }

  // Selection AABBs.
  for (const Box2d& aabbDoc : snapshot.aabbsDoc) {
    AppendRect(&out, aabbDoc, "none", kOverlayStroke, "1", kOverlayLineClass);
  }

  // Oriented rotation box (drawn instead of axis-aligned AABBs during rotation).
  if (snapshot.orientedBoundsDoc.has_value()) {
    AppendOrientedBox(&out, snapshot.orientedBoundsDoc->cornersDoc);
  }

  // Resize handles: small filled squares.
  for (const Vector2d& handleAnchorDoc : snapshot.handleAnchorsDoc) {
    AppendRect(&out,
               OverlayRenderer::ChromeSquareForPoint(
                   snapshot, OverlayRenderer::ChromeSquare::TransformHandle, handleAnchorDoc),
               kOverlayHandleFill, kOverlayStroke, "1", kOverlayHandleClass);
  }

  // Marquee rect.
  if (snapshot.marqueeDoc.has_value()) {
    AppendRect(&out, *snapshot.marqueeDoc, "none", kOverlayStroke, "1", kOverlayLineClass);
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

  const std::string_view source = doc.source();

  // Tokenize once with the shared XML tokenizer; root bounds, root attributes,
  // and the external-reference check below all derive from this stream.
  const std::vector<xml::XMLToken> tokens = TokenizeSource(source);

  // Refuse documents that reference external resources we cannot embed safely.
  const std::string externalReference = FindExternalReference(source, tokens);
  if (!externalReference.empty()) {
    return ResultType::Err(
        "Viewport export cannot embed external resource reference: " + externalReference +
        ". Inline or remove external href/xlink:href references and try again.");
  }

  const RootTag rootTag = ParseRootTag(source, tokens);
  if (!rootTag.found) {
    return ResultType::Err("Viewport export could not find the root <svg> element in the source.");
  }

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
  output += "<svg";
  bool wroteViewBox = false;
  bool wroteWidth = false;
  bool wroteHeight = false;
  for (const Attribute& attribute : rootTag.attributes) {
    if (attribute.name == "viewBox") {
      output += " viewBox=\"" + EscapeXml(viewBoxValue) + "\"";
      wroteViewBox = true;
    } else if (attribute.name == "width") {
      output += " width=\"" + std::to_string(outputWidth) + "\"";
      wroteWidth = true;
    } else if (attribute.name == "height") {
      output += " height=\"" + std::to_string(outputHeight) + "\"";
      wroteHeight = true;
    } else {
      output += " " + attribute.name + "=\"" + EscapeXml(attribute.value) + "\"";
    }
  }
  if (!wroteWidth) {
    output += " width=\"" + std::to_string(outputWidth) + "\"";
  }
  if (!wroteHeight) {
    output += " height=\"" + std::to_string(outputHeight) + "\"";
  }
  if (!wroteViewBox) {
    output += " viewBox=\"" + EscapeXml(viewBoxValue) + "\"";
  }
  output += ">\n";

  // Defs: clip path covering the document-space viewport rect.
  output += "  <defs><clipPath id=\"";
  output += clipPathId;
  output += "\"><rect x=\"" + FormatNumber(viewBoxMinX) + "\" y=\"" + FormatNumber(viewBoxMinY) +
            "\" width=\"" + FormatNumber(viewBoxWidth) + "\" height=\"" +
            FormatNumber(viewBoxHeight) + "\"/></clipPath></defs>\n";

  // Optional covering background rect (non-transparent export).
  if (!options.transparentBackground) {
    output += "  <rect x=\"" + FormatNumber(viewBoxMinX) + "\" y=\"" + FormatNumber(viewBoxMinY) +
              "\" width=\"" + FormatNumber(viewBoxWidth) + "\" height=\"" +
              FormatNumber(viewBoxHeight) + "\" fill=\"#ffffff\"/>\n";
  }

  // Source stylesheets remain active in the exported SVG. Define the editor
  // chrome rules before the imported content so Donner's stylesheet traversal
  // applies them last, while the scoped selector and !important declarations
  // also give them the intended precedence in standards-compliant browsers.
  if (options.includeSelectionOverlay && overlaySnapshot != nullptr) {
    output += "  ";
    output += OverlayStylesheet(overlayGroupId);
    output += "\n";
  }

  // Document content: source children verbatim, wrapped in a clipped group.
  output += "  <g clip-path=\"url(#";
  output += clipPathId;
  output += ")\">";
  if (rootTag.bodyEnd > rootTag.bodyStart) {
    output += source.substr(rootTag.bodyStart, rootTag.bodyEnd - rootTag.bodyStart);
  }
  output += "</g>\n";

  // Optional editor overlay group. Populated from the captured selection-chrome
  // snapshot when one is supplied; otherwise emitted empty (back-compat).
  // Clipped to the same document-space viewport rect as the content so overlay
  // chrome never spills outside the exported crop.
  if (options.includeSelectionOverlay) {
    output += "  <g id=\"";
    output += overlayGroupId;
    output +=
        "\" data-donner-export-role=\"editor-overlay\" pointer-events=\"none\" clip-path=\"url(#";
    output += clipPathId;
    output += ")\">";
    if (overlaySnapshot != nullptr) {
      output += SerializeOverlaySnapshotToSvg(*overlaySnapshot);
    }
    output += "</g>\n";
  }

  output += "</svg>\n";

  return ResultType::Ok(std::move(output));
}

}  // namespace donner::editor
